#pragma once
// Process-wide owner of the libVIIPER USB server, its bus, and the single
// shared virtual keyboard/mouse pair.
//
// Why this type exists
// -------------------------------------------------------------------------
// libVIIPER's USB server binds a fixed TCP address, so at most one server can
// exist per process. Before this fork every VirtualController created its own
// server, bus, keyboard and mouse in its constructor, which meant:
//
//   * a second controller could never be emulated — the second server collided
//     on localhost:3245 and failed only after ten 150 ms retries; and
//   * N controllers would have produced N virtual keyboards and N virtual mice,
//     when what you want is one of each for the whole app.
//
// Hoisting the shared plumbing out fixes both. It also removes the teardown
// race that made switching emulation modes slow and error-prone: the server is
// created once and kept until Shutdown(), so a mode switch now only swaps the
// pad device instead of tearing down and re-racing the listen socket.
//
// The keyboard and mouse are reference counted alongside the virtual pads, so
// no phantom input devices linger in Device Manager while steamless mode is off.

#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4505)
#endif
#include "libVIIPER/libVIIPER.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// Resolved libVIIPER entry points, populated once on first use. Check `loaded`
// before calling anything, and `mouseLoaded` / `keyboardLoaded` before the
// optional HID entry points — older libVIIPER builds do not export them.
struct ViiperApi {
    void* module = nullptr;
    decltype(&NewUSBServer) NewUSBServerFn = nullptr;
    decltype(&CloseUSBServer) CloseUSBServerFn = nullptr;
    decltype(&CreateUSBBus) CreateUSBBusFn = nullptr;
    decltype(&RemoveUSBBus) RemoveUSBBusFn = nullptr;
    decltype(&CreateXbox360Device) CreateXbox360DeviceFn = nullptr;
    decltype(&SetXbox360DeviceState) SetXbox360DeviceStateFn = nullptr;
    decltype(&SetXbox360RumbleCallback) SetXbox360RumbleCallbackFn = nullptr;
    decltype(&RemoveXbox360Device) RemoveXbox360DeviceFn = nullptr;
    decltype(&CreateDS4Device) CreateDS4DeviceFn = nullptr;
    decltype(&SetDS4DeviceState) SetDS4DeviceStateFn = nullptr;
    decltype(&SetDS4OutputCallback) SetDS4OutputCallbackFn = nullptr;
    decltype(&RemoveDS4Device) RemoveDS4DeviceFn = nullptr;
    decltype(&CreateMouseDevice) CreateMouseDeviceFn = nullptr;
    decltype(&SetMouseDeviceState) SetMouseDeviceStateFn = nullptr;
    decltype(&RemoveMouseDevice) RemoveMouseDeviceFn = nullptr;
    decltype(&CreateKeyboardDevice) CreateKeyboardDeviceFn = nullptr;
    decltype(&SetKeyboardDeviceState) SetKeyboardDeviceStateFn = nullptr;
    decltype(&RemoveKeyboardDevice) RemoveKeyboardDeviceFn = nullptr;
    bool loaded = false;
    bool mouseLoaded = false;
    bool keyboardLoaded = false;
};

// Loads libVIIPER.dll from the application directory on first call and resolves
// its exports. Safe to call from any thread; the load happens exactly once.
ViiperApi& GetViiperApi();

class ViiperBus {
public:
    static ViiperBus& Instance();

    // Ensures the server and bus exist and the shared keyboard/mouse are
    // present. Reference counted — call exactly once per virtual pad, and pair
    // every successful call with a Release(). Returns false when libVIIPER or
    // the USBIP driver is unavailable, in which case IsDriverMissing() is set.
    //
    // Cold-start note: this can block for up to ~1.5 s the first time while it
    // retries a server that the USBIP driver has not finished initialising.
    // Callers on the UI thread should move construction to a worker.
    bool Acquire();

    // Drops one pad reference. At zero the shared keyboard and mouse are
    // removed so they do not linger while emulation is off; the server itself
    // is deliberately kept alive for fast re-acquisition.
    void Release();

    // Closes the server. Call once at process exit, after every pad is gone.
    void Shutdown();

    bool IsDriverMissing() const;

    std::uintptr_t ServerHandle() const;
    std::uint32_t  BusId() const;

    // Shared virtual HID keyboard. Chords are additive: holding two mappings
    // that share a modifier keeps that modifier down until both release.
    void KeyChordDown(const std::vector<std::uint16_t>& vkChord);
    void KeyChordUp(const std::vector<std::uint16_t>& vkChord);

    // Clears every held key/modifier. Used when yielding the controller so a
    // key cannot stay latched down after we stop emulating.
    void ReleaseAllKeys();

    // Shared virtual HID mouse. Falls back to SendInput when libVIIPER has no
    // mouse device (older builds, or creation failed).
    void UpdateMouse(std::int16_t dx, std::int16_t dy, std::uint8_t buttons);

private:
    ViiperBus() = default;
    ~ViiperBus() = default;
    ViiperBus(const ViiperBus&) = delete;
    ViiperBus& operator=(const ViiperBus&) = delete;

    // All *Locked helpers require m_mutex to be held by the caller.
    bool EnsureServerLocked();
    void CreateHidDevicesLocked();
    void RemoveHidDevicesLocked();
    void ApplyKeyVkLocked(std::uint16_t vk, bool down);
    void PushKeyboardStateLocked();

    mutable std::mutex m_mutex;
    int            m_refCount      = 0;
    std::uintptr_t m_serverHandle  = 0;
    std::uint32_t  m_busId         = 0;
    bool           m_serverReady   = false;
    bool           m_driverMissing = false;

    std::uintptr_t m_keyboardHandle = 0;
    std::uintptr_t m_mouseHandle    = 0;
    std::uint8_t   m_kbModifiers    = 0;
    std::array<std::uint8_t, 32> m_kbBitmap{};
    std::uint8_t   m_lastMouseButtons = 0;
};

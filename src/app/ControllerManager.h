#pragma once
#include "PaddleOverlay.h"
#include "SdlGamepadInput.h"
#include "TrackpadMouse.h"
#include "VirtualController.h"
#include <functional>
#include <thread>
#include <atomic>
#include <memory>
#include <cstdint>
#include <array>
#include <mutex>
#include <string>
#include <atomic>

// Manages the Steam Controller lifecycle: device discovery, lizard mode
// disable/enable, and the heartbeat that keeps lizard mode off.
// All public methods are safe to call from the UI thread.
class ControllerManager {
public:
    struct UiNavigationState {
        bool up = false;
        bool down = false;
        bool left = false;
        bool right = false;
        bool confirm = false;
        bool back = false;
        bool previous = false;
        bool next = false;
        bool clear = false;
        bool record = false;
    };

    using StateChangedFn = std::function<void(bool connected, bool gameModeActive, bool outputBackendMissing)>;

    explicit ControllerManager(StateChangedFn onStateChanged);
    ~ControllerManager();
    ControllerManager(const ControllerManager&) = delete;
    ControllerManager& operator=(const ControllerManager&) = delete;

    // Called when Windows reports a device arrival or removal (WM_DEVICECHANGE),
    // and also from the reconnect retry timer.
    //
    // Pass deviceArrival=true only for a real WM_DEVICECHANGE arrival. It clears
    // the liveness-probe cooldowns, so hardware that has just appeared or woken
    // is probed immediately instead of waiting one out. The retry timer must pass
    // false, or the cooldown never takes effect and every retry re-probes every
    // silent dongle slot.
    void OnDeviceChange(bool deviceArrival = false);

    // Forgets liveness-probe cooldowns without attempting to open anything.
    // For callers that know circumstances changed — a Steam state transition,
    // resume from suspend — but must not themselves claim the device.
    void ClearProbeCooldowns();
    void OnSuspend();
    void OnResume();
    void RecoverIfInputStalled();

    // Toggle game mode on/off. No-op if controller is not connected.
    void EnableGameMode();
    void DisableGameMode();

    // Disables game mode *and* closes the HID handle so another process can
    // claim the controller. DisableGameMode alone is not enough to hand over:
    // it restores lizard mode but keeps our exclusive handle, so Steam Input
    // would still find the device busy. Safe to call when already disabled.
    void ReleaseDevices();

    // Detach/reattach the virtual controller while keeping game mode active.
    // Used to let a game initialize its input stack before VIIPER appears.
    void DetachVirtual();
    void AttachVirtual();

    void SetTrackpadMouseEnabled(bool enabled);
    void SetBackButtonsEnabled(bool enabled);
    void SetUseLeftTrackpad(bool enabled);
    void SetEmulationMode(EmulationMode mode);
    void SetPaddleMapping(int paddleIndex, PaddleMapping mapping);
    void SetPaddleActions(PaddleActionBindings actions);

    // True while we hold write access exclusively, i.e. Steam Input cannot drive
    // the controller. False means we are coexisting with Steam and it may also
    // be acting on the pad.
    bool HasExclusiveAccess()      const { return m_hasExclusiveAccess; }

    bool IsConnected()             const { return m_connected; }
    bool IsGameModeActive()        const { return m_gameModeActive; }
    bool IsOutputBackendMissing()  const { return m_outputBackendMissing; }
    bool IsTrackpadMouseEnabled()  const { return m_trackpadMouseEnabled; }
    bool IsBackButtonsEnabled()    const { return m_backButtonsEnabled; }
    bool IsUseLeftTrackpad()       const { return m_useLeftTrackpad; }
    EmulationMode GetEmulationMode() const { return m_emulationMode; }
    PaddleMappings GetPaddleMappings() const { return m_paddleMappings; }
    PaddleActionBindings GetPaddleActions() const { return m_paddleActions; }
    std::wstring GetCurrentMacroCaptureChord() const;
    UiNavigationState GetUiNavigationState() const;
    std::wstring GetControllerReportSignature() const;
    int GetBatteryPercent() const;

private:
    StandardGamepadState GetLatestStandardState() const;
    void TryOpen();
    void Close(bool restoreLizard);
    void StartReadLoop();
    void StopReadLoop();
    void ReadLoop();
    void PulseTrackpadClickHaptics(uint8_t strength);
    void ReleaseExclusiveIfHeld();

    StateChangedFn                     m_onStateChanged;
    bool                               m_connected            = false;
    bool                               m_gameModeActive       = false;
    bool                               m_hasExclusiveAccess   = false;
    bool                               m_outputBackendMissing = false;
    bool                               m_trackpadMouseEnabled = true;
    bool                               m_backButtonsEnabled   = false;
    bool                               m_useLeftTrackpad      = false;
    EmulationMode                      m_emulationMode        = EmulationMode::Xbox360;
    PaddleMappings                     m_paddleMappings{};
    PaddleActionBindings               m_paddleActions{};
    std::unique_ptr<VirtualController> m_virtual;
    PaddleOverlay                      m_paddleOverlay;
    TrackpadMouse                      m_trackpad;
    std::thread                        m_readThread;
    std::atomic<bool>                  m_readRunning{false};
    std::atomic<std::uint64_t>         m_lastReportTickMs{0};
    std::atomic<std::uint64_t>         m_lastTrackpadHapticPulseTickMs{0};
    std::uint64_t                      m_lastDeviceChangeLogTickMs = 0;
    bool                               m_lastDeviceChangeLogConnected = false;
    bool                               m_lastDeviceChangeLogGameMode = false;
    mutable std::mutex                 m_lastReportMutex;
    std::array<uint8_t, 64>            m_lastReport{};
    size_t                             m_lastReportSize = 0;
    mutable std::mutex                 m_standardStateMutex;
    StandardGamepadState               m_lastStandardState{};
    SdlGamepadInput                    m_sdlInput;
};

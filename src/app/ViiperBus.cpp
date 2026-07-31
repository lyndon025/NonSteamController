#include "ViiperBus.h"
#include "logging/Log.h"

#include <Windows.h>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

namespace {

// libVIIPER listens on a fixed address, which is exactly why only one server
// may exist per process. Kept as a mutable buffer because USBServerConfig::addr
// is a non-const char* and writing through a cast-away string literal is UB.
char g_serverAddr[] = "localhost:3245";

constexpr int  kServerAttempts    = 10;
constexpr int  kServerRetryDelayMs = 150;

template <typename T>
bool LoadProc(HMODULE module, const char* name, T& fn) {
    fn = reinterpret_cast<T>(GetProcAddress(module, name));
    if (!fn)
        logging::Logf("[VIIPER] Missing export: %s", name);
    return fn != nullptr;
}

std::wstring GetAppDirectory() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring full(path);
    const size_t slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return L".";
    return full.substr(0, slash);
}

void ViiperLogCallback(VIIPERLogLevel level, const char* message) {
    const char* levelName = "INFO";
    switch (level) {
    case VIIPER_LOG_DEBUG: levelName = "DEBUG"; break;
    case VIIPER_LOG_INFO:  levelName = "INFO";  break;
    case VIIPER_LOG_WARN:  levelName = "WARN";  break;
    case VIIPER_LOG_ERROR: levelName = "ERROR"; break;
    }
    logging::Logf("[VIIPER/%s] %s", levelName, message ? message : "");
}

// Returns the HID modifier bit for modifier VK codes, or 0 if not a modifier.
uint8_t VkToModifierBit(uint16_t vk) {
    switch (vk) {
    case VK_LCONTROL: case VK_CONTROL: return KB_MOD_LEFT_CTRL;
    case VK_RCONTROL:                  return KB_MOD_RIGHT_CTRL;
    case VK_LSHIFT:   case VK_SHIFT:   return KB_MOD_LEFT_SHIFT;
    case VK_RSHIFT:                    return KB_MOD_RIGHT_SHIFT;
    case VK_LMENU:    case VK_MENU:    return KB_MOD_LEFT_ALT;
    case VK_RMENU:                     return KB_MOD_RIGHT_ALT;
    case VK_LWIN:                      return KB_MOD_LEFT_GUI;
    case VK_RWIN:                      return KB_MOD_RIGHT_GUI;
    default:                           return 0;
    }
}

// Returns the HID usage code for a non-modifier VK, or 0 if unknown.
uint8_t VkToHidUsage(uint16_t vk) {
    // Letters A-Z
    if (vk >= 'A' && vk <= 'Z') return static_cast<uint8_t>(0x04 + (vk - 'A'));
    // Digits 1-9, then 0
    if (vk >= '1' && vk <= '9') return static_cast<uint8_t>(0x1E + (vk - '1'));
    if (vk == '0') return 0x27;
    // Function keys F1-F12
    if (vk >= VK_F1  && vk <= VK_F12) return static_cast<uint8_t>(0x3A + (vk - VK_F1));
    if (vk >= VK_F13 && vk <= VK_F24) return static_cast<uint8_t>(0x68 + (vk - VK_F13));
    switch (vk) {
    case VK_RETURN:    return 0x28;
    case VK_ESCAPE:    return 0x29;
    case VK_BACK:      return 0x2A;
    case VK_TAB:       return 0x2B;
    case VK_SPACE:     return 0x2C;
    case VK_OEM_MINUS: return 0x2D;
    case VK_OEM_PLUS:  return 0x2E;
    case VK_OEM_4:     return 0x2F;  // [
    case VK_OEM_6:     return 0x30;  // ]
    case VK_OEM_5:     return 0x31;  // backslash
    case VK_OEM_1:     return 0x33;  // ;
    case VK_OEM_7:     return 0x34;  // '
    case VK_OEM_3:     return 0x35;  // `
    case VK_OEM_COMMA: return 0x36;
    case VK_OEM_PERIOD:return 0x37;
    case VK_OEM_2:     return 0x38;  // /
    case VK_CAPITAL:   return 0x39;
    case VK_SNAPSHOT:  return 0x46;
    case VK_SCROLL:    return 0x47;
    case VK_PAUSE:     return 0x48;
    case VK_INSERT:    return 0x49;
    case VK_HOME:      return 0x4A;
    case VK_PRIOR:     return 0x4B;  // Page Up
    case VK_DELETE:    return 0x4C;
    case VK_END:       return 0x4D;
    case VK_NEXT:      return 0x4E;  // Page Down
    case VK_RIGHT:     return 0x4F;
    case VK_LEFT:      return 0x50;
    case VK_DOWN:      return 0x51;
    case VK_UP:        return 0x52;
    case VK_NUMLOCK:   return 0x53;
    case VK_DIVIDE:    return 0x54;
    case VK_MULTIPLY:  return 0x55;
    case VK_SUBTRACT:  return 0x56;
    case VK_ADD:       return 0x57;
    case VK_NUMPAD1:   return 0x59;
    case VK_NUMPAD2:   return 0x5A;
    case VK_NUMPAD3:   return 0x5B;
    case VK_NUMPAD4:   return 0x5C;
    case VK_NUMPAD5:   return 0x5D;
    case VK_NUMPAD6:   return 0x5E;
    case VK_NUMPAD7:   return 0x5F;
    case VK_NUMPAD8:   return 0x60;
    case VK_NUMPAD9:   return 0x61;
    case VK_NUMPAD0:   return 0x62;
    case VK_DECIMAL:   return 0x63;
    default:           return 0;
    }
}

}  // namespace

ViiperApi& GetViiperApi() {
    static ViiperApi api;
    static std::once_flag once;
    std::call_once(once, [&]() {
        const std::wstring dllPath = GetAppDirectory() + L"\\libVIIPER.dll";
        HMODULE module = LoadLibraryW(dllPath.c_str());
        api.module = module;
        if (!module) {
            logging::Logf("[VIIPER] LoadLibrary failed path=%s error=%lu",
                          logging::Narrow(dllPath).c_str(),
                          GetLastError());
            return;
        }

        api.loaded =
            LoadProc(module, "NewUSBServer", api.NewUSBServerFn) &&
            LoadProc(module, "CloseUSBServer", api.CloseUSBServerFn) &&
            LoadProc(module, "CreateUSBBus", api.CreateUSBBusFn) &&
            LoadProc(module, "RemoveUSBBus", api.RemoveUSBBusFn) &&
            LoadProc(module, "CreateXbox360Device", api.CreateXbox360DeviceFn) &&
            LoadProc(module, "SetXbox360DeviceState", api.SetXbox360DeviceStateFn) &&
            LoadProc(module, "SetXbox360RumbleCallback", api.SetXbox360RumbleCallbackFn) &&
            LoadProc(module, "RemoveXbox360Device", api.RemoveXbox360DeviceFn) &&
            LoadProc(module, "CreateDS4Device", api.CreateDS4DeviceFn) &&
            LoadProc(module, "SetDS4DeviceState", api.SetDS4DeviceStateFn) &&
            LoadProc(module, "SetDS4OutputCallback", api.SetDS4OutputCallbackFn) &&
            LoadProc(module, "RemoveDS4Device", api.RemoveDS4DeviceFn);

        if (api.loaded) {
            api.mouseLoaded =
                LoadProc(module, "CreateMouseDevice",   api.CreateMouseDeviceFn) &&
                LoadProc(module, "SetMouseDeviceState", api.SetMouseDeviceStateFn) &&
                LoadProc(module, "RemoveMouseDevice",   api.RemoveMouseDeviceFn);
            api.keyboardLoaded =
                LoadProc(module, "CreateKeyboardDevice",   api.CreateKeyboardDeviceFn) &&
                LoadProc(module, "SetKeyboardDeviceState", api.SetKeyboardDeviceStateFn) &&
                LoadProc(module, "RemoveKeyboardDevice",   api.RemoveKeyboardDeviceFn);
            logging::Logf("[VIIPER] libVIIPER loaded from %s mouseSupport=%d keyboardSupport=%d",
                          logging::Narrow(dllPath).c_str(),
                          api.mouseLoaded ? 1 : 0, api.keyboardLoaded ? 1 : 0);
        }
    });
    return api;
}

ViiperBus& ViiperBus::Instance() {
    static ViiperBus bus;
    return bus;
}

bool ViiperBus::EnsureServerLocked() {
    if (m_serverReady)
        return true;

    ViiperApi& api = GetViiperApi();
    if (!api.loaded) {
        logging::Logf("[VIIPER] API not available");
        m_driverMissing = true;
        return false;
    }

    USBServerConfig config{};
    config.addr = g_serverAddr;
    config.connection_timeout_ms = 30000;
    config.device_handler_connect_timeout_ms = 5000;
    config.write_batch_flush_interval_ms = 1;

    // Retried because on a cold boot the USBIP driver may not have finished
    // initialising when we first ask for a server. Unlike the pre-fork code
    // this runs at most once per process, not on every emulation-mode switch.
    for (int attempt = 0; attempt < kServerAttempts; ++attempt) {
        if (!api.NewUSBServerFn(&config, &m_serverHandle, &ViiperLogCallback)) {
            m_serverHandle = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(kServerRetryDelayMs));
            continue;
        }
        if (!api.CreateUSBBusFn(m_serverHandle, &m_busId)) {
            api.CloseUSBServerFn(m_serverHandle);
            m_serverHandle = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(kServerRetryDelayMs));
            continue;
        }
        m_serverReady   = true;
        m_driverMissing = false;
        logging::Logf("[VIIPER] Server ready addr=%s bus=%u (attempt %d)",
                      g_serverAddr, m_busId, attempt + 1);
        return true;
    }

    logging::Logf("[VIIPER] Server/bus creation failed after %d attempts; "
                  "USBIP client/driver may be unavailable", kServerAttempts);
    m_driverMissing = true;
    return false;
}

void ViiperBus::CreateHidDevicesLocked() {
    ViiperApi& api = GetViiperApi();

    if (api.mouseLoaded && !m_mouseHandle) {
        if (api.CreateMouseDeviceFn(m_serverHandle, &m_mouseHandle, m_busId, true, 0, 0) != 0)
            logging::Logf("[VIIPER] Shared virtual mouse connected handle=%llu",
                          static_cast<unsigned long long>(m_mouseHandle));
        else {
            logging::Logf("[VIIPER] Shared virtual mouse creation failed");
            m_mouseHandle = 0;
        }
    }

    if (api.keyboardLoaded && !m_keyboardHandle) {
        if (api.CreateKeyboardDeviceFn(m_serverHandle, &m_keyboardHandle, m_busId, true, 0, 0) != 0)
            logging::Logf("[VIIPER] Shared virtual keyboard connected handle=%llu",
                          static_cast<unsigned long long>(m_keyboardHandle));
        else {
            logging::Logf("[VIIPER] Shared virtual keyboard creation failed");
            m_keyboardHandle = 0;
        }
    }
}

void ViiperBus::RemoveHidDevicesLocked() {
    ViiperApi& api = GetViiperApi();
    if (!api.loaded)
        return;

    if (api.keyboardLoaded && m_keyboardHandle) {
        // Drop anything still held so a latched key cannot survive the removal.
        m_kbModifiers = 0;
        m_kbBitmap.fill(0);
        PushKeyboardStateLocked();
        api.RemoveKeyboardDeviceFn(m_keyboardHandle);
        m_keyboardHandle = 0;
    }
    if (api.mouseLoaded && m_mouseHandle) {
        api.RemoveMouseDeviceFn(m_mouseHandle);
        m_mouseHandle = 0;
    }
    m_lastMouseButtons = 0;
}

bool ViiperBus::Acquire() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!EnsureServerLocked())
        return false;

    ++m_refCount;
    CreateHidDevicesLocked();
    logging::Logf("[VIIPER] Bus acquired refCount=%d", m_refCount);
    return true;
}

void ViiperBus::Release() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_refCount <= 0)
        return;

    if (--m_refCount == 0)
        RemoveHidDevicesLocked();
    logging::Logf("[VIIPER] Bus released refCount=%d", m_refCount);
}

void ViiperBus::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    ViiperApi& api = GetViiperApi();

    RemoveHidDevicesLocked();
    m_refCount = 0;

    if (api.loaded && m_serverReady && m_serverHandle) {
        // Let the bus finish processing the device removals above before the
        // server goes away; closing underneath the async USB cleanup makes
        // libVIIPER surface an error dialog.
        Sleep(150);
        api.CloseUSBServerFn(m_serverHandle);
        logging::Logf("[VIIPER] Server closed");
    }
    m_serverHandle = 0;
    m_busId        = 0;
    m_serverReady  = false;
}

bool ViiperBus::IsDriverMissing() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_driverMissing;
}

std::uintptr_t ViiperBus::ServerHandle() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_serverHandle;
}

std::uint32_t ViiperBus::BusId() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_busId;
}

void ViiperBus::ApplyKeyVkLocked(std::uint16_t vk, bool down) {
    const uint8_t mod = VkToModifierBit(vk);
    if (mod) {
        if (down) m_kbModifiers |= mod;
        else      m_kbModifiers = static_cast<std::uint8_t>(m_kbModifiers & ~mod);
        return;
    }
    const uint8_t hid = VkToHidUsage(vk);
    if (!hid) return;
    if (down) m_kbBitmap[hid / 8] |=  static_cast<uint8_t>(1u << (hid % 8));
    else      m_kbBitmap[hid / 8] &= static_cast<uint8_t>(~(1u << (hid % 8)));
}

void ViiperBus::PushKeyboardStateLocked() {
    ViiperApi& api = GetViiperApi();
    if (!api.keyboardLoaded || !m_keyboardHandle)
        return;
    KeyboardDeviceState state{};
    state.Modifiers = m_kbModifiers;
    memcpy(state.KeyBitmap, m_kbBitmap.data(), sizeof(state.KeyBitmap));
    api.SetKeyboardDeviceStateFn(m_keyboardHandle, state);
}

void ViiperBus::KeyChordDown(const std::vector<std::uint16_t>& vkChord) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_keyboardHandle) return;
    for (std::uint16_t vk : vkChord) ApplyKeyVkLocked(vk, true);
    PushKeyboardStateLocked();
}

void ViiperBus::KeyChordUp(const std::vector<std::uint16_t>& vkChord) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_keyboardHandle) return;
    for (std::uint16_t vk : vkChord) ApplyKeyVkLocked(vk, false);
    PushKeyboardStateLocked();
}

void ViiperBus::ReleaseAllKeys() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_keyboardHandle) return;
    m_kbModifiers = 0;
    m_kbBitmap.fill(0);
    PushKeyboardStateLocked();
}

void ViiperBus::UpdateMouse(std::int16_t dx, std::int16_t dy, std::uint8_t buttons) {
    std::uint8_t changed = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ViiperApi& api = GetViiperApi();
        if (m_mouseHandle && api.mouseLoaded) {
            MouseDeviceState state{};
            state.Buttons = buttons;
            state.DX = dx;
            state.DY = dy;
            api.SetMouseDeviceStateFn(m_mouseHandle, state);
            m_lastMouseButtons = buttons;
            return;
        }
        // No libVIIPER mouse — fall through to SendInput. Work out the button
        // edges while still holding the lock so m_lastMouseButtons is never
        // touched unsynchronised (RemoveHidDevicesLocked also resets it).
        changed = static_cast<std::uint8_t>(buttons ^ m_lastMouseButtons);
        m_lastMouseButtons = buttons;
    }

    // SendInput is issued outside the lock: it can block on the target thread's
    // input queue, and holding the bus lock through that would stall pad output.
    if (dx != 0 || dy != 0) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        input.mi.dx = dx;
        input.mi.dy = dy;
        SendInput(1, &input, sizeof(INPUT));
    }
    if (changed & 0x01u) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = (buttons & 0x01u) ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        SendInput(1, &input, sizeof(INPUT));
    }
    if (changed & 0x02u) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = (buttons & 0x02u) ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        SendInput(1, &input, sizeof(INPUT));
    }
}

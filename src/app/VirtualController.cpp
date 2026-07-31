#include "VirtualController.h"
#include "ViiperBus.h"
#include "logging/Log.h"
#include "steam/SteamController.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

std::mutex g_notificationMutex;
std::unordered_map<std::uintptr_t, VirtualController*> g_targetOwners;

static XusbReport Translate(const uint8_t* buf, size_t n) {
    constexpr int16_t kStickCenterDeadzone = 1024;
    XusbReport r{};
    if (!SteamController::UsesLegacyStateLayout(buf, n)) return r;

    const uint8_t b0 = buf[2];
    const uint8_t b1 = buf[3];
    const uint8_t b2 = buf[4];

    if (b0 & SteamController::BTN_A) r.buttons |= XUSB_GAMEPAD_A;
    if (b0 & SteamController::BTN_B) r.buttons |= XUSB_GAMEPAD_B;
    if (b0 & SteamController::BTN_X) r.buttons |= XUSB_GAMEPAD_X;
    if (b0 & SteamController::BTN_Y) r.buttons |= XUSB_GAMEPAD_Y;
    if (b2 & SteamController::BTN_LB) r.buttons |= XUSB_GAMEPAD_LEFT_SHOULDER;
    if (b1 & SteamController::BTN_RB) r.buttons |= XUSB_GAMEPAD_RIGHT_SHOULDER;
    if (b0 & SteamController::BTN_MENU) r.buttons |= XUSB_GAMEPAD_START;
    if (b1 & SteamController::BTN_VIEW) r.buttons |= XUSB_GAMEPAD_BACK;
    if (b1 & SteamController::BTN_LS) r.buttons |= XUSB_GAMEPAD_LEFT_THUMB;
    if (b0 & SteamController::BTN_RS) r.buttons |= XUSB_GAMEPAD_RIGHT_THUMB;
    if (b2 & SteamController::BTN_STEAM) r.buttons |= XUSB_GAMEPAD_GUIDE;
    if (b1 & SteamController::BTN_DPAD_UP)  r.buttons |= XUSB_GAMEPAD_DPAD_UP;
    if (b1 & SteamController::BTN_DPAD_DN)  r.buttons |= XUSB_GAMEPAD_DPAD_DOWN;
    if (b1 & SteamController::BTN_DPAD_LT)  r.buttons |= XUSB_GAMEPAD_DPAD_LEFT;
    if (b1 & SteamController::BTN_DPAD_RT)  r.buttons |= XUSB_GAMEPAD_DPAD_RIGHT;

    int16_t ltRaw = 0;
    int16_t rtRaw = 0;
    memcpy(&ltRaw, buf + 6, 2);
    memcpy(&rtRaw, buf + 8, 2);
    r.leftTrigger  = static_cast<uint8_t>(std::clamp<int>(ltRaw >> 7, 0, 255));
    r.rightTrigger = static_cast<uint8_t>(std::clamp<int>(rtRaw >> 7, 0, 255));

    memcpy(&r.leftX, buf + 10, 2);
    memcpy(&r.leftY, buf + 12, 2);
    memcpy(&r.rightX, buf + 14, 2);
    memcpy(&r.rightY, buf + 16, 2);
    if (std::abs(static_cast<int>(r.leftX)) < kStickCenterDeadzone) r.leftX = 0;
    if (std::abs(static_cast<int>(r.leftY)) < kStickCenterDeadzone) r.leftY = 0;
    if (std::abs(static_cast<int>(r.rightX)) < kStickCenterDeadzone) r.rightX = 0;
    if (std::abs(static_cast<int>(r.rightY)) < kStickCenterDeadzone) r.rightY = 0;
    return r;
}

static XusbReport Translate(const StandardGamepadState& state) {
    XusbReport r{};
    if (!state.connected)
        return r;

    if (state.a) r.buttons |= XUSB_GAMEPAD_A;
    if (state.b) r.buttons |= XUSB_GAMEPAD_B;
    if (state.x) r.buttons |= XUSB_GAMEPAD_X;
    if (state.y) r.buttons |= XUSB_GAMEPAD_Y;
    if (state.leftShoulder) r.buttons |= XUSB_GAMEPAD_LEFT_SHOULDER;
    if (state.rightShoulder) r.buttons |= XUSB_GAMEPAD_RIGHT_SHOULDER;
    if (state.start) r.buttons |= XUSB_GAMEPAD_START;
    if (state.back) r.buttons |= XUSB_GAMEPAD_BACK;
    if (state.leftStick) r.buttons |= XUSB_GAMEPAD_LEFT_THUMB;
    if (state.rightStick) r.buttons |= XUSB_GAMEPAD_RIGHT_THUMB;
    if (state.guide) r.buttons |= XUSB_GAMEPAD_GUIDE;
    if (state.dpadUp) r.buttons |= XUSB_GAMEPAD_DPAD_UP;
    if (state.dpadDown) r.buttons |= XUSB_GAMEPAD_DPAD_DOWN;
    if (state.dpadLeft) r.buttons |= XUSB_GAMEPAD_DPAD_LEFT;
    if (state.dpadRight) r.buttons |= XUSB_GAMEPAD_DPAD_RIGHT;
    r.leftTrigger = state.leftTrigger;
    r.rightTrigger = state.rightTrigger;
    r.leftX = state.leftX;
    r.leftY = state.leftY;
    r.rightX = state.rightX;
    r.rightY = state.rightY;
    return r;
}

void ApplyPaddleMapping(XusbReport& report, PaddleMapping mapping) {
    switch (mapping) {
    case PaddleMapping::None: return;
    case PaddleMapping::A: report.buttons |= XUSB_GAMEPAD_A; return;
    case PaddleMapping::B: report.buttons |= XUSB_GAMEPAD_B; return;
    case PaddleMapping::X: report.buttons |= XUSB_GAMEPAD_X; return;
    case PaddleMapping::Y: report.buttons |= XUSB_GAMEPAD_Y; return;
    case PaddleMapping::LeftShoulder: report.buttons |= XUSB_GAMEPAD_LEFT_SHOULDER; return;
    case PaddleMapping::RightShoulder: report.buttons |= XUSB_GAMEPAD_RIGHT_SHOULDER; return;
    case PaddleMapping::View: report.buttons |= XUSB_GAMEPAD_BACK; return;
    case PaddleMapping::Menu: report.buttons |= XUSB_GAMEPAD_START; return;
    case PaddleMapping::LeftThumb: report.buttons |= XUSB_GAMEPAD_LEFT_THUMB; return;
    case PaddleMapping::RightThumb: report.buttons |= XUSB_GAMEPAD_RIGHT_THUMB; return;
    case PaddleMapping::Guide: report.buttons |= XUSB_GAMEPAD_GUIDE; return;
    case PaddleMapping::DPadUp: report.buttons |= XUSB_GAMEPAD_DPAD_UP; return;
    case PaddleMapping::DPadRight: report.buttons |= XUSB_GAMEPAD_DPAD_RIGHT; return;
    case PaddleMapping::DPadDown: report.buttons |= XUSB_GAMEPAD_DPAD_DOWN; return;
    case PaddleMapping::DPadLeft: report.buttons |= XUSB_GAMEPAD_DPAD_LEFT; return;
    }
}

PaddleMapping ResolvePaddleGamepadMapping(PaddleMapping menuMapping, const PaddleAction& action) {
    switch (action.type) {
    case PaddleActionType::UseMenuMapping:
        return menuMapping;
    case PaddleActionType::None:
    case PaddleActionType::KeyChord:
    case PaddleActionType::Macro:
        return PaddleMapping::None;
    case PaddleActionType::Gamepad:
        return action.gamepadMappings.empty() ? PaddleMapping::None : action.gamepadMappings[0];
    }
    return PaddleMapping::None;
}

static bool IsButtonPressed(const uint8_t* buf, size_t n, int index,
                             const StandardGamepadState* standardState) {
    if (standardState && standardState->connected) {
        switch (index) {
        case  0: return standardState->leftPaddle1;
        case  1: return standardState->leftPaddle2;
        case  2: return standardState->rightPaddle1;
        case  3: return standardState->rightPaddle2;
        case  4: return standardState->misc1 || standardState->touchpadButton;
        case  5: return standardState->a;
        case  6: return standardState->b;
        case  7: return standardState->x;
        case  8: return standardState->y;
        case  9: return standardState->leftShoulder;
        case 10: return standardState->rightShoulder;
        case 11: return standardState->back;
        case 12: return standardState->start;
        case 13: return standardState->guide;
        case 14: return standardState->leftStick;
        case 15: return standardState->rightStick;
        case 16: return standardState->dpadUp;
        case 17: return standardState->dpadDown;
        case 18: return standardState->dpadLeft;
        case 19: return standardState->dpadRight;
        case 20: return standardState->leftTrigger  > 50;
        case 21: return standardState->rightTrigger > 50;
        default: return false;
        }
    }
    if (!SteamController::UsesLegacyStateLayout(buf, n))
        return false;
    const uint8_t b0 = buf[2];
    const uint8_t b1 = buf[3];
    const uint8_t b2 = buf[4];
    switch (index) {
    case  0: return (b2 & SteamController::BTN_L4)       != 0;
    case  1: return (b2 & SteamController::BTN_L5)       != 0;
    case  2: return (b0 & SteamController::BTN_R4)       != 0;
    case  3: return (b1 & SteamController::BTN_R5)       != 0;
    case  4: return (b0 & SteamController::BTN_QAM)      != 0;
    case  5: return (b0 & SteamController::BTN_A)        != 0;
    case  6: return (b0 & SteamController::BTN_B)        != 0;
    case  7: return (b0 & SteamController::BTN_X)        != 0;
    case  8: return (b0 & SteamController::BTN_Y)        != 0;
    case  9: return (b2 & SteamController::BTN_LB)       != 0;
    case 10: return (b1 & SteamController::BTN_RB)       != 0;
    case 11: return (b1 & SteamController::BTN_VIEW)     != 0;
    case 12: return (b0 & SteamController::BTN_MENU)     != 0;
    case 13: return (b2 & SteamController::BTN_STEAM)    != 0;
    case 14: return (b1 & SteamController::BTN_LS)       != 0;
    case 15: return (b0 & SteamController::BTN_RS)       != 0;
    case 16: return (b1 & SteamController::BTN_DPAD_UP)  != 0;
    case 17: return (b1 & SteamController::BTN_DPAD_DN)  != 0;
    case 18: return (b1 & SteamController::BTN_DPAD_LT)  != 0;
    case 19: return (b1 & SteamController::BTN_DPAD_RT)  != 0;
    // Triggers: 16-bit LE at buf[6..7] (LT) and buf[8..9] (RT), range 0–0x7FFF.
    case 20: return n > 7  && (static_cast<int16_t>(buf[6]  | (buf[7]  << 8))) > 0x1000;
    case 21: return n > 9  && (static_cast<int16_t>(buf[8]  | (buf[9]  << 8))) > 0x1000;
    default: return false;
    }
}

// Returns the XusbReport button bit for a standard button (index 5–19), or 0.
static uint16_t GetStandardButtonXusbBit(int index) {
    switch (index) {
    case  5: return XUSB_GAMEPAD_A;
    case  6: return XUSB_GAMEPAD_B;
    case  7: return XUSB_GAMEPAD_X;
    case  8: return XUSB_GAMEPAD_Y;
    case  9: return XUSB_GAMEPAD_LEFT_SHOULDER;
    case 10: return XUSB_GAMEPAD_RIGHT_SHOULDER;
    case 11: return XUSB_GAMEPAD_BACK;
    case 12: return XUSB_GAMEPAD_START;
    case 13: return XUSB_GAMEPAD_GUIDE;
    case 14: return XUSB_GAMEPAD_LEFT_THUMB;
    case 15: return XUSB_GAMEPAD_RIGHT_THUMB;
    case 16: return XUSB_GAMEPAD_DPAD_UP;
    case 17: return XUSB_GAMEPAD_DPAD_DOWN;
    case 18: return XUSB_GAMEPAD_DPAD_LEFT;
    case 19: return XUSB_GAMEPAD_DPAD_RIGHT;
    default: return 0;
    }
}

void ApplyAllButtonRemaps(XusbReport& report, const uint8_t* buf, size_t n,
                          const StandardGamepadState* standardState,
                          const PaddleMappings& mappings,
                          const PaddleActionBindings& actions,
                          bool prevPressed[kTotalButtonCount]) {
    const PaddleMapping paddleMenuMappings[] = {
        mappings.l4, mappings.l5, mappings.r4, mappings.r5, mappings.qam
    };

    for (int i = 0; i < kTotalButtonCount; ++i) {
        const bool pressed = IsButtonPressed(buf, n, i, standardState);
        const PaddleAction* actionPtr = GetButtonAction(actions, i);
        const PaddleAction& action = actionPtr ? *actionPtr : PaddleAction{};

        if (i < kPaddleCount) {
            // Paddles: apply gamepad mapping to the XusbReport when pressed.
            const PaddleMapping mapping = ResolvePaddleGamepadMapping(paddleMenuMappings[i], action);
            const bool active = pressed && (
                action.rapidFire ? ((GetTickCount64() / 90) % 2 == 0) :
                (action.type == PaddleActionType::UseMenuMapping ||
                 action.type == PaddleActionType::Gamepad));
            if (active)
                ApplyPaddleMapping(report, mapping);
        } else if (i == 20 || i == 21) {
            // Triggers: suppress the analog axis and optionally remap.
            if (action.type != PaddleActionType::UseMenuMapping) {
                uint8_t& triggerAxis = (i == 20) ? report.leftTrigger : report.rightTrigger;
                triggerAxis = 0;
                if (action.type == PaddleActionType::Gamepad && pressed) {
                    const bool rapidOk = !action.rapidFire || (GetTickCount64() / 90) % 2 == 0;
                    if (rapidOk)
                        for (PaddleMapping m : action.gamepadMappings) ApplyPaddleMapping(report, m);
                }
            }
        } else {
            // Standard buttons: suppress from XusbReport and apply the remap
            // when the action is anything other than UseMenuMapping (passthrough).
            if (action.type != PaddleActionType::UseMenuMapping) {
                const uint16_t originalBit = GetStandardButtonXusbBit(i);
                report.buttons &= static_cast<uint16_t>(~originalBit); // suppress original

                if (action.type == PaddleActionType::Gamepad && pressed) {
                    const bool rapidOk = !action.rapidFire || (GetTickCount64() / 90) % 2 == 0;
                    if (rapidOk)
                        for (PaddleMapping m : action.gamepadMappings) ApplyPaddleMapping(report, m);
                }
                // KeyChord / Macro / None: bit cleared; PaddleOverlay fires key events.
            }
        }
        prevPressed[i] = pressed;
    }
}

int8_t ScaleThumbToDs4(int16_t value) {
    const int clamped = std::clamp(static_cast<int>(std::lround(static_cast<double>(value) * 127.0 / 32767.0)), -128, 127);
    return static_cast<int8_t>(clamped);
}

uint8_t BuildDs4Dpad(const XusbReport& xusb) {
    const bool up    = (xusb.buttons & XUSB_GAMEPAD_DPAD_UP)    != 0;
    const bool down  = (xusb.buttons & XUSB_GAMEPAD_DPAD_DOWN)  != 0;
    const bool left  = (xusb.buttons & XUSB_GAMEPAD_DPAD_LEFT)  != 0;
    const bool right = (xusb.buttons & XUSB_GAMEPAD_DPAD_RIGHT) != 0;

    if (up   && right) return DS4_DPAD_UP_RIGHT;
    if (down && right) return DS4_DPAD_DOWN_RIGHT;
    if (down && left)  return DS4_DPAD_DOWN_LEFT;
    if (up   && left)  return DS4_DPAD_UP_LEFT;
    if (up)    return DS4_DPAD_UP;
    if (right) return DS4_DPAD_RIGHT;
    if (down)  return DS4_DPAD_DOWN;
    if (left)  return DS4_DPAD_LEFT;
    return DS4_DPAD_NEUTRAL;
}

DS4DeviceState TranslateDs4(const XusbReport& xusb) {
    DS4DeviceState ds4{};
    // XInput sticks are +Y = up; DualShock 4 axes are +Y = down. Negate the
    // vertical axes so up/down are not inverted on the virtual DS4 pad.
    ds4.LX = ScaleThumbToDs4(xusb.leftX);
    ds4.LY = ScaleThumbToDs4(static_cast<int16_t>(-std::clamp<int>(xusb.leftY, -32767, 32767)));
    ds4.RX = ScaleThumbToDs4(xusb.rightX);
    ds4.RY = ScaleThumbToDs4(static_cast<int16_t>(-std::clamp<int>(xusb.rightY, -32767, 32767)));
    ds4.DPad = BuildDs4Dpad(xusb);
    ds4.L2 = xusb.leftTrigger;
    ds4.R2 = xusb.rightTrigger;
    ds4.AccelZ = static_cast<int16_t>(-9.81f * 512.0f);

    if (xusb.buttons & XUSB_GAMEPAD_BACK) ds4.Buttons |= DS4_BUTTON_SHARE;
    if (xusb.buttons & XUSB_GAMEPAD_START) ds4.Buttons |= DS4_BUTTON_OPTIONS;
    if (xusb.buttons & XUSB_GAMEPAD_LEFT_THUMB) ds4.Buttons |= DS4_BUTTON_L3;
    if (xusb.buttons & XUSB_GAMEPAD_RIGHT_THUMB) ds4.Buttons |= DS4_BUTTON_R3;
    if (xusb.buttons & XUSB_GAMEPAD_LEFT_SHOULDER) ds4.Buttons |= DS4_BUTTON_L1;
    if (xusb.buttons & XUSB_GAMEPAD_RIGHT_SHOULDER) ds4.Buttons |= DS4_BUTTON_R1;
    if (xusb.buttons & XUSB_GAMEPAD_GUIDE) ds4.Buttons |= DS4_BUTTON_PS;
    if (xusb.buttons & XUSB_GAMEPAD_A) ds4.Buttons |= DS4_BUTTON_CROSS;
    if (xusb.buttons & XUSB_GAMEPAD_B) ds4.Buttons |= DS4_BUTTON_CIRCLE;
    if (xusb.buttons & XUSB_GAMEPAD_X) ds4.Buttons |= DS4_BUTTON_SQUARE;
    if (xusb.buttons & XUSB_GAMEPAD_Y) ds4.Buttons |= DS4_BUTTON_TRIANGLE;
    if (xusb.leftTrigger > 0) ds4.Buttons |= DS4_BUTTON_L2;
    if (xusb.rightTrigger > 0) ds4.Buttons |= DS4_BUTTON_R2;
    return ds4;
}

} // namespace

VirtualController::VirtualController(EmulationMode mode, PaddleMappings paddleMappings,
                                     PaddleActionBindings paddleActions,
                                     RumbleCallback onRumble)
    : m_mode(mode), m_paddleMappings(paddleMappings), m_paddleActions(std::move(paddleActions)),
      m_onRumble(std::move(onRumble)) {
    ViiperBus& bus = ViiperBus::Instance();
    if (!bus.Acquire()) {
        logging::Logf("[VIIPER] Bus unavailable; cannot create virtual pad");
        m_driverMissing = true;
        return;
    }
    m_busAcquired = true;

    ViiperApi& api = GetViiperApi();
    const std::uintptr_t server = bus.ServerHandle();
    const std::uint32_t  busId  = bus.BusId();

    bool ok = false;
    if (m_mode == EmulationMode::DualShock4) {
        ok = api.CreateDS4DeviceFn(server, &m_deviceHandle, busId, true, 0, 0, nullptr) != 0;
        if (ok)
            ok = api.SetDS4OutputCallbackFn(m_deviceHandle, &VirtualController::ViiperDs4OutputCallback) != 0;
    } else {
        ok = api.CreateXbox360DeviceFn(server, &m_deviceHandle, busId, true, 0, 0, 0) != 0;
        if (ok)
            ok = api.SetXbox360RumbleCallbackFn(m_deviceHandle, &VirtualController::ViiperXboxRumbleCallback) != 0;
    }

    if (!ok) {
        logging::Logf("[VIIPER] Device creation/register callback failed mode=%d", static_cast<int>(m_mode));
        m_driverMissing = true;
        if (m_deviceHandle) {
            if (m_mode == EmulationMode::DualShock4) api.RemoveDS4DeviceFn(m_deviceHandle);
            else                                     api.RemoveXbox360DeviceFn(m_deviceHandle);
            m_deviceHandle = 0;
        }
        // The server stays up — it is shared and other pads may still need it.
        bus.Release();
        m_busAcquired = false;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_notificationMutex);
        g_targetOwners[m_deviceHandle] = this;
    }

    logging::Logf("[VIIPER] Virtual %s controller connected bus=%u handle=%llu",
                  m_mode == EmulationMode::DualShock4 ? "DualShock 4" : "Xbox 360",
                  busId,
                  static_cast<unsigned long long>(m_deviceHandle));
    m_valid = true;
}

VirtualController::~VirtualController() {
    logging::Logf("[VIIPER] VirtualController dtor valid=%d", m_valid ? 1 : 0);
    ViiperApi& api = GetViiperApi();

    // Erased under the same lock the callbacks take, so a rumble callback that
    // is already mid-lookup finishes before this object's storage goes away.
    if (m_deviceHandle) {
        std::lock_guard<std::mutex> lock(g_notificationMutex);
        g_targetOwners.erase(m_deviceHandle);
    }

    if (api.loaded && m_deviceHandle) {
        if (m_mode == EmulationMode::DualShock4) {
            api.SetDS4OutputCallbackFn(m_deviceHandle, nullptr);
            api.RemoveDS4DeviceFn(m_deviceHandle);
        } else {
            api.SetXbox360RumbleCallbackFn(m_deviceHandle, nullptr);
            api.RemoveXbox360DeviceFn(m_deviceHandle);
        }
        m_deviceHandle = 0;
    }

    // Only drops the shared keyboard/mouse once the last pad is gone; the
    // server itself is closed by ViiperBus::Shutdown() at process exit. This is
    // what makes an emulation-mode switch cheap instead of a full server cycle.
    if (m_busAcquired) {
        ViiperBus::Instance().Release();
        m_busAcquired = false;
    }
}

void VirtualController::Update(const uint8_t* buf, size_t n, const StandardGamepadState* standardState) {
    if (!m_valid) return;

    XusbReport xusb{};
    if (standardState && standardState->connected) {
        xusb = Translate(*standardState);
        if (!m_loggedSdlState) {
            logging::Logf("[SDL] Using SDL standard gamepad state for virtual controller translation");
            m_loggedSdlState = true;
        }
    } else {
        xusb = Translate(buf, n);
        m_loggedSdlState = false;
    }

    ApplyAllButtonRemaps(xusb, buf, n, standardState, m_paddleMappings, m_paddleActions, m_prevPaddlePressed);

    {
        std::lock_guard<std::mutex> lock(m_macroMutex);
        m_lastBaseXusbReport = xusb;
        xusb.buttons |= m_macroGamepadButtons;
    }

    SendXusbReport(xusb);
}

void VirtualController::SendXusbReport(const XusbReport& xusb) {
    ViiperApi& api = GetViiperApi();
    if (m_mode == EmulationMode::DualShock4) {
        DS4DeviceState state = TranslateDs4(xusb);
        api.SetDS4DeviceStateFn(m_deviceHandle, state);
    } else {
        Xbox360DeviceState state{};
        state.Buttons = xusb.buttons;
        state.LT = xusb.leftTrigger;
        state.RT = xusb.rightTrigger;
        state.LX = xusb.leftX;
        state.LY = xusb.leftY;
        state.RX = xusb.rightX;
        state.RY = xusb.rightY;
        api.SetXbox360DeviceStateFn(m_deviceHandle, state);
    }
}

void VirtualController::ViiperXboxRumbleCallback(std::uintptr_t handle, uint8_t leftMotor, uint8_t rightMotor) {
    std::lock_guard<std::mutex> lock(g_notificationMutex);
    auto it = g_targetOwners.find(handle);
    if (it == g_targetOwners.end() || !it->second->m_onRumble)
        return;
    it->second->m_onRumble(leftMotor, rightMotor);
}

// Keyboard and mouse output live on the shared bus, so these are thin
// forwards. They stay on VirtualController because PaddleOverlay and
// TrackpadMouse are already wired to the pad that owns the binding.
void VirtualController::KeyChordDown(const std::vector<uint16_t>& vkChord) {
    ViiperBus::Instance().KeyChordDown(vkChord);
}

void VirtualController::KeyChordUp(const std::vector<uint16_t>& vkChord) {
    ViiperBus::Instance().KeyChordUp(vkChord);
}

void VirtualController::UpdateMouse(int16_t dx, int16_t dy, uint8_t buttons) {
    ViiperBus::Instance().UpdateMouse(dx, dy, buttons);
}

void VirtualController::GamepadMacroDown(const std::vector<PaddleMapping>& mappings) {
    XusbReport report;
    {
        std::lock_guard<std::mutex> lock(m_macroMutex);
        XusbReport tmp{};
        for (PaddleMapping m : mappings) ApplyPaddleMapping(tmp, m);
        m_macroGamepadButtons |= tmp.buttons;
        report = m_lastBaseXusbReport;
        report.buttons |= m_macroGamepadButtons;
    }
    SendXusbReport(report);
}

void VirtualController::GamepadMacroUp(const std::vector<PaddleMapping>& mappings) {
    XusbReport report;
    {
        std::lock_guard<std::mutex> lock(m_macroMutex);
        XusbReport tmp{};
        for (PaddleMapping m : mappings) ApplyPaddleMapping(tmp, m);
        m_macroGamepadButtons &= ~tmp.buttons;
        report = m_lastBaseXusbReport;
        report.buttons |= m_macroGamepadButtons;
    }
    SendXusbReport(report);
}

void VirtualController::ViiperDs4OutputCallback(std::uintptr_t handle, uint8_t rumbleSmall, uint8_t rumbleLarge,
                                                uint8_t ledRed, uint8_t ledGreen, uint8_t ledBlue,
                                                uint8_t flashOn, uint8_t flashOff) {
    (void)ledRed;
    (void)ledGreen;
    (void)ledBlue;
    (void)flashOn;
    (void)flashOff;

    std::lock_guard<std::mutex> lock(g_notificationMutex);
    auto it = g_targetOwners.find(handle);
    if (it == g_targetOwners.end() || !it->second->m_onRumble)
        return;
    it->second->m_onRumble(rumbleLarge, rumbleSmall);
}

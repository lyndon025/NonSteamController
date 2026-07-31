#include "SteamController.h"
#include "hid/DeviceIdentity.h"
#include "logging/Log.h"
#include <algorithm>
#include <chrono>
#include <array>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>

// ---------------------------------------------------------------------------
// Internal helper: build a 64-byte feature report command buffer.
//
// The 2026 Steam Controller routes firmware commands through Feature Report
// 0x01 (same channel the original SC used, now with an explicit report ID).
//
// Buffer layout:
//   [0] FEATURE_REPORT_CMD (0x01)  — HID feature report ID
//   [1] cmd                         — command byte (0x81, 0x87, etc.)
//   [2] payloadSize                 — number of payload bytes that follow
//   [3..3+payloadSize-1] payload    — command arguments
//   [rest] zeros
// ---------------------------------------------------------------------------

static void BuildCmd(uint8_t (&buf)[64], uint8_t cmd,
                     const uint8_t* payload = nullptr, uint8_t payloadSize = 0) {
    std::memset(buf, 0, 64);
    buf[0] = SteamController::FEATURE_REPORT_CMD;
    buf[1] = cmd;
    buf[2] = payloadSize;
    if (payload && payloadSize)
        std::memcpy(buf + 3, payload, payloadSize);
}

template <typename T>
static void AppendSetting(T& payload, uint8_t& count, uint8_t setting, uint16_t value) {
    payload[count++] = setting;
    payload[count++] = static_cast<uint8_t>(value & 0xFF);
    payload[count++] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

namespace {

std::wstring BuildReportSignature(uint16_t pid, uint8_t reportId, size_t size, SteamController::InputReportKind kind) {
    wchar_t buffer[64] = {};
    swprintf_s(buffer, L"pid=%04X;report=%02X;size=%zu;kind=%d",
               pid,
               static_cast<unsigned>(reportId),
               size,
               static_cast<int>(kind));
    return buffer;
}

std::string HexDump(const uint8_t* data, size_t size, size_t maxBytes = 48) {
    std::ostringstream out;
    const size_t bytes = (std::min)(size, maxBytes);
    for (size_t i = 0; i < bytes; ++i) {
        if (i != 0)
            out << ' ';
        char byte[4];
        std::snprintf(byte, sizeof(byte), "%02X", data[i]);
        out << byte;
    }
    if (size > bytes)
        out << " ...";
    return out.str();
}

void LogUnknownInputReportSample(const uint8_t* data, size_t size, const char* reason) {
    static std::mutex mutex;
    static std::set<std::string> seen;

    std::ostringstream key;
    key << reason << "|" << size << "|";
    const size_t prefixBytes = (std::min)(size, static_cast<size_t>(8));
    for (size_t i = 0; i < prefixBytes; ++i) {
        char byte[4];
        std::snprintf(byte, sizeof(byte), "%02X", data[i]);
        key << byte;
    }

    std::lock_guard<std::mutex> lock(mutex);
    if (!seen.insert(key.str()).second)
        return;

    logging::Logf("[SteamController] Unknown input report reason=%s size=%zu bytes=%s",
                  reason, size, HexDump(data, size).c_str());
}

} // namespace

SteamController::InputReportKind SteamController::ClassifyInputReport(const uint8_t* buffer, size_t size) {
    if (!buffer || size == 0)
        return InputReportKind::None;

    const uint8_t reportId = buffer[0];
    if (reportId == REPORT_STATE && size >= 30)
        return InputReportKind::LegacyState;

    if (reportId == REPORT_STATUS || reportId == REPORT_UNKNOWN_79 || reportId == REPORT_UNKNOWN_7B)
        return InputReportKind::NonState;

    // Be tolerant of firmware changes that keep the same payload shape but change
    // the report ID or transport framing. If we have enough bytes for the legacy
    // parser and the report is not one of the known housekeeping packets, treat it
    // as state-compatible and let the higher layer try the legacy offsets.
    if (size >= 30)
        return InputReportKind::CompatibleState;

    return InputReportKind::NonState;
}

// ---------------------------------------------------------------------------
// Open / Close
// ---------------------------------------------------------------------------

std::vector<SteamController::Candidate> SteamController::EnumerateCandidates() {
    std::vector<Candidate> candidates;
    std::set<std::wstring> seenPaths;

    // Wired before dongle: a cabled controller reports over USB and its dongle
    // slot falls silent, so trying wired first avoids burning a probe timeout on
    // the quiet one.
    for (uint16_t pid : { SC2026_PID, SC2026_DONGLE_PID }) {
        const bool wired = (pid == SC2026_PID);
        const auto paths = HidDevice::Enumerate(VALVE_VID, pid, VENDOR_USAGE_PAGE);
        logging::Logf("[SteamController] Enumerate pid=%04X paths=%zu", pid, paths.size());

        for (const auto& path : paths) {
            // Deliberately NOT deduplicated by physical device. The dongle's
            // pairing slots are several HID interfaces of a single USB device,
            // so they share one container ID — collapsing on that would throw
            // away three of four slots and break multi-controller over one
            // dongle. What separates a real controller from an idle slot is
            // liveness, which Open() establishes with its report probe.
            //
            // Paths are still deduplicated (case-insensitively) as cheap
            // insurance against the same interface being listed twice.
            std::wstring normalized = path;
            std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                           [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            if (!seenPaths.insert(normalized).second) {
                logging::Logf("[SteamController] Skipping duplicate path pid=%04X path=%s",
                              pid, logging::Narrow(path).c_str());
                continue;
            }

            // physicalKey deliberately left empty. Computing it here cost a
            // CM_Get_Device_Interface_Property pair plus a CreateFileW for every
            // candidate, on every sweep — and this runs from the reconnect timer
            // on the UI thread, where it showed up as lag opening the tray menu.
            // Nothing consumes the key yet; callers that need identity can ask
            // DeviceIdentity directly for the one path they care about.
            candidates.push_back(Candidate{path, std::wstring(), pid, wired});
        }
    }

    logging::Logf("[SteamController] EnumerateCandidates -> %zu interface(s)",
                  candidates.size());
    return candidates;
}

bool SteamController::Open(const Candidate& candidate) {
    // Presence is not liveness: the dongle exposes a slot per pairing whether or
    // not a controller is awake on it. Require a state-shaped report before
    // claiming the interface, otherwise a sleeping slot becomes a virtual pad
    // that never moves.
    constexpr uint32_t kProbeTimeoutMs = 80;

    logging::Logf("[SteamController] Trying path pid=%04X path=%s",
                  candidate.pid, logging::Narrow(candidate.path).c_str());
    if (!m_device.Open(candidate.path))
        return false;

    uint8_t buf[64];
    const size_t n = m_device.ReadInputReport(buf, sizeof(buf), kProbeTimeoutMs);
    const InputReportKind kind = ClassifyInputReport(buf, n);
    if (kind == InputReportKind::LegacyState || kind == InputReportKind::CompatibleState) {
        m_devicePath          = candidate.path;
        m_lastReportSignature = BuildReportSignature(candidate.pid, n > 0 ? buf[0] : 0, n, kind);
        logging::Logf("[SteamController] Active interface pid=%04X reportBytes=%zu reportId=0x%02X kind=%d",
                      candidate.pid, n, n > 0 ? buf[0] : 0, static_cast<int>(kind));
        return true;
    }

    logging::Logf("[SteamController] Path rejected pid=%04X reportBytes=%zu reportId=0x%02X kind=%d",
                  candidate.pid, n, n > 0 ? buf[0] : 0, static_cast<int>(kind));
    if (n > 0)
        LogUnknownInputReportSample(buf, n, "open-probe");

    m_device.Close();
    return false;
}

bool SteamController::Open() {
    logging::Logf("[SteamController] Open begin");

    // Every candidate is probed on every attempt, deliberately.
    //
    // There used to be a per-path cooldown here to keep this loop cheap, but it
    // could only ever delay detection: a controller that was asleep when first
    // probed stayed unreachable until the cooldown expired. The cost it existed
    // to avoid is now handled by sweeping far less often (see
    // RECONNECT_BACKOFF_MS) and by not doing identity lookups per candidate, so
    // one mechanism replaces two that interacted badly.
    for (const auto& candidate : EnumerateCandidates()) {
        if (Open(candidate))
            return true;
    }

    logging::Logf("[SteamController] Open failed (no live interface; wired PID=%04X, dongle PID=%04X)",
                  SC2026_PID, SC2026_DONGLE_PID);
    return false;
}

static uint16_t ScaleMotorToAmplitude(uint8_t motor) {
    // Map XInput's 0-255 motor range into the 0-0x7FFF range used by the
    // Steam Controller haptic packet. Zero stays silent.
    return static_cast<uint16_t>((static_cast<uint32_t>(motor) * 0x7FFFu) / 0xFFu);
}

// ---------------------------------------------------------------------------
// Exclusive access control
//
// Ported from SteamlessController, whose model this follows: shared while idle
// so Steam can coexist and we can still track connect/disconnect, exclusive
// only while actively emulating, shared again on yield. Denying write access is
// what stops Steam Input's Desktop Configuration driving the pad underneath us.
// ---------------------------------------------------------------------------

bool SteamController::ClaimExclusive() {
    // FILE_SHARE_READ without FILE_SHARE_WRITE: others may read, none may write.
    if (m_device.Reopen(FILE_SHARE_READ)) {
        logging::Logf("[SteamController] ClaimExclusive success");
        return true;
    }
    // Another process holds a write handle. Reopen already closed our old one,
    // so restore shared access rather than leaving the device shut.
    m_device.Reopen(FILE_SHARE_READ | FILE_SHARE_WRITE);
    logging::Logf("[SteamController] ClaimExclusive failed — another process holds write access");
    return false;
}

void SteamController::ReleaseToShared() {
    m_device.Reopen(FILE_SHARE_READ | FILE_SHARE_WRITE);
    logging::Logf("[SteamController] ReleaseToShared");
}

void SteamController::Close() {
    logging::Logf("[SteamController] Close");
    if (m_running.exchange(false) && m_heartbeat.joinable())
        m_heartbeat.join();
    {
        std::lock_guard<std::mutex> lock(m_rumbleMutex);
        m_rumbleStop = true;
        m_largeMotor = 0;
        m_smallMotor = 0;
    }
    m_rumbleCv.notify_all();
    if (m_rumbleThread.joinable())
        m_rumbleThread.join();
    m_device.Close();
    m_devicePath.clear();
}

// ---------------------------------------------------------------------------
// Lizard mode
// ---------------------------------------------------------------------------

bool SteamController::DisableLizardMode() {
    uint8_t buf[64];
    logging::Logf("[SteamController] DisableLizardMode begin");

    // Step 1: CLEAR_DIGITAL_MAPPINGS — kills keyboard/mouse button emulation.
    BuildCmd(buf, CMD_CLEAR_DIGITAL_MAPPINGS);
    {
        std::lock_guard<std::mutex> lock(m_featureMutex);
        if (!m_device.SendFeatureReport(buf, sizeof(buf))) {
            printf("Failed to send CLEAR_DIGITAL_MAPPINGS.\n");
            logging::Logf("[SteamController] DisableLizardMode failed step=CLEAR_DIGITAL_MAPPINGS");
            return false;
        }
    }

    // Step 2: LOAD_DEFAULT_SETTINGS — start from the controller defaults before
    // applying our Steam-like mouse profile.
    BuildCmd(buf, CMD_LOAD_DEFAULT_SETTINGS);
    {
        std::lock_guard<std::mutex> lock(m_featureMutex);
        if (!m_device.SendFeatureReport(buf, sizeof(buf))) {
            printf("Failed to send LOAD_DEFAULT_SETTINGS.\n");
            logging::Logf("[SteamController] DisableLizardMode failed step=LOAD_DEFAULT_SETTINGS");
            return false;
        }
    }

    if (!ApplyTrackpadMouseSettings()) {
        logging::Logf("[SteamController] DisableLizardMode failed step=APPLY_TRACKPAD_MOUSE_SETTINGS");
        return false;
    }

    if (!m_running.exchange(true))
        m_heartbeat = std::thread(&SteamController::HeartbeatLoop, this);

    {
        std::lock_guard<std::mutex> lock(m_rumbleMutex);
        m_rumbleStop = false;
    }

    logging::Logf("[SteamController] DisableLizardMode success");
    return true;
}

bool SteamController::EnableLizardMode() {
    logging::Logf("[SteamController] EnableLizardMode begin");
    if (m_running.exchange(false) && m_heartbeat.joinable())
        m_heartbeat.join();

    uint8_t buf[64];
    BuildCmd(buf, CMD_SET_DEFAULT_MAPPINGS);
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(m_featureMutex);
        ok = m_device.SendFeatureReport(buf, sizeof(buf));
    }
    logging::Logf("[SteamController] EnableLizardMode %s", ok ? "success" : "failed");
    return ok;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

size_t SteamController::ReadReport(uint8_t* buffer, size_t size, uint32_t timeoutMs) {
    return m_device.ReadInputReport(buffer, size, timeoutMs);
}

void SteamController::SetRumble(uint8_t largeMotor, uint8_t smallMotor) {
    {
        std::lock_guard<std::mutex> lock(m_rumbleMutex);
        m_largeMotor = largeMotor;
        m_smallMotor = smallMotor;
        if (!m_rumbleThread.joinable()) {
            m_rumbleStop = false;
            m_rumbleThread = std::thread(&SteamController::RumbleLoop, this);
        }
    }

    if (largeMotor != 0 || smallMotor != 0) {
        logging::Logf("[SteamController] SetRumble large=%u small=%u",
                      static_cast<unsigned>(largeMotor),
                      static_cast<unsigned>(smallMotor));
    }
    m_rumbleCv.notify_one();
}

void SteamController::PulseHaptic(uint8_t strength) {
    if (strength == 0)
        return;

    // Use the right actuator for a short UI click pulse without entering the
    // persistent rumble loop that virtual controller feedback uses.
    SendHapticCommand(/*right=*/0, ScaleMotorToAmplitude(strength), /*period=*/0, /*count=*/1);
}

void SteamController::SetDesktopTrackpadMouseMode(bool enabled, bool useLeftTrackpad) {
    m_trackpadMouseEnabled = enabled;
    m_useLeftTrackpadMouse = useLeftTrackpad;
    if (m_running.load() && m_device.IsOpen())
        ApplyTrackpadMouseSettings();
}


// ---------------------------------------------------------------------------
// Heartbeat
// ---------------------------------------------------------------------------

void SteamController::HeartbeatLoop() {
    uint8_t buf[64];
    BuildCmd(buf, CMD_CLEAR_DIGITAL_MAPPINGS);
    logging::Logf("[SteamController] Heartbeat start");

    while (m_running.load()) {
        {
            std::lock_guard<std::mutex> lock(m_featureMutex);
            m_device.SendFeatureReport(buf, sizeof(buf));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }

    logging::Logf("[SteamController] Heartbeat stop");
}

bool SteamController::SendHapticCommand(uint8_t position, uint16_t amplitude, uint16_t period, uint16_t count) {
    (void)position;
    (void)amplitude;
    (void)period;
    (void)count;

    // This 4-byte interrupt-OUT packet is the confirmed working actuator trigger,
    // captured from Steam's own haptics path. The 0x8F feature report approach
    // was tried previously and did not reliably fire the actuator.
    const uint8_t outPacket[] = { 0x82, 0x00, 0x02, 0x00 };

    std::lock_guard<std::mutex> lock(m_featureMutex);
    bool ok = m_device.WriteOutputPacket(outPacket, sizeof(outPacket));
    return ok;
}

bool SteamController::ApplyTrackpadMouseSettings() {
    uint8_t payload[32] = {};
    uint8_t payloadSize = 0;

    AppendSetting(payload, payloadSize, SETTING_WIRELESS_PACKET_VERSION, 2);
    AppendSetting(payload, payloadSize, SETTING_LEFT_TRACKPAD_MODE,
                  (m_trackpadMouseEnabled && m_useLeftTrackpadMouse) ? TRACKPAD_ABSOLUTE_MOUSE : TRACKPAD_NONE);
    AppendSetting(payload, payloadSize, SETTING_RIGHT_TRACKPAD_MODE,
                  (m_trackpadMouseEnabled && !m_useLeftTrackpadMouse) ? TRACKPAD_ABSOLUTE_MOUSE : TRACKPAD_NONE);
    AppendSetting(payload, payloadSize, SETTING_SMOOTH_ABSOLUTE_MOUSE, m_trackpadMouseEnabled ? 1 : 0);
    AppendSetting(payload, payloadSize, SETTING_MOMENTUM_MAXIMUM_VELOCITY, m_trackpadMouseEnabled ? 20000 : 8000);
    AppendSetting(payload, payloadSize, SETTING_MOMENTUM_DECAY_AMOUNT, m_trackpadMouseEnabled ? 50 : 5);

    uint8_t buf[64];
    BuildCmd(buf, CMD_SET_SETTINGS, payload, payloadSize);
    std::lock_guard<std::mutex> lock(m_featureMutex);
    const bool ok = m_device.SendFeatureReport(buf, sizeof(buf));
    logging::Logf("[SteamController] ApplyTrackpadMouseSettings enabled=%d left=%d payloadBytes=%u ok=%d",
                  m_trackpadMouseEnabled ? 1 : 0,
                  m_useLeftTrackpadMouse ? 1 : 0,
                  static_cast<unsigned>(payloadSize),
                  ok ? 1 : 0);
    return ok;
}

void SteamController::RumbleLoop() {
    logging::Logf("[SteamController] RumbleLoop start");
    std::unique_lock<std::mutex> lock(m_rumbleMutex);

    while (!m_rumbleStop) {
        m_rumbleCv.wait(lock, [&] {
            return m_rumbleStop || m_largeMotor != 0 || m_smallMotor != 0;
        });

        while (!m_rumbleStop && (m_largeMotor != 0 || m_smallMotor != 0)) {
            const uint8_t largeMotor = m_largeMotor;
            const uint8_t smallMotor = m_smallMotor;
            lock.unlock();

            // Since the haptic packet fires at fixed amplitude, simulate variable
            // intensity via pulse rate modulation: stronger motor → faster pulses.
            // Large motor (left actuator) drives low-frequency rumble feel.
            // Small motor (right actuator) drives high-frequency buzz feel.
            if (largeMotor != 0)
                SendHapticCommand(/*left=*/1, ScaleMotorToAmplitude(largeMotor), 0, 1);
            if (smallMotor != 0)
                SendHapticCommand(/*right=*/0, ScaleMotorToAmplitude(smallMotor), 0, 1);

            // Pulse interval: 6ms (strong) → 20ms (weak), mapped from 255 → 1.
            const uint8_t dominant = (largeMotor > smallMotor) ? largeMotor : smallMotor;
            const int intervalMs = 20 - static_cast<int>((dominant * 14) / 255);
            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
            lock.lock();
        }
    }

    logging::Logf("[SteamController] RumbleLoop stop");
}

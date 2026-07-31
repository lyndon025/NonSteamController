#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdint>
#include <string>
#include <vector>

class HidDevice {
public:
    // Returns device paths for all HID interfaces matching vid/pid/usagePage.
    // Pass usagePage=0 to return all matching interfaces.
    static std::vector<std::wstring> Enumerate(uint16_t vid, uint16_t pid, uint16_t usagePage = 0);

    HidDevice() = default;
    ~HidDevice() { Close(); }
    HidDevice(const HidDevice&) = delete;
    HidDevice& operator=(const HidDevice&) = delete;
    HidDevice(HidDevice&& o) noexcept;
    HidDevice& operator=(HidDevice&& o) noexcept;

    // Opens with shared access (FILE_SHARE_READ | FILE_SHARE_WRITE), so another
    // process such as Steam can hold the device at the same time. Use Reopen to
    // change the share mode afterwards.
    bool Open(const std::wstring& path);
    void Close();
    bool IsOpen() const { return m_handle != INVALID_HANDLE_VALUE; }

    // Reopens the same path with a different share mode, keeping the event and
    // cached report lengths (same physical device, so they still apply).
    //
    // Passing FILE_SHARE_READ alone denies other processes write access, which
    // is how Steam is kept from driving the controller while we are emulating.
    // Fails if another process already holds write access.
    //
    // The caller MUST ensure no read thread is running: this cancels pending
    // overlapped I/O and swaps the handle out from under it otherwise.
    bool Reopen(DWORD shareMode);

    // Send a HID output report (interrupt OUT / SET_REPORT Output type).
    // data[0] must be the report ID. Padded to OutputReportByteLength automatically.
    bool SendOutputReport(const uint8_t* data, size_t size);

    // Write raw bytes to the HID interrupt-OUT endpoint without report-length padding.
    bool WriteOutputPacket(const uint8_t* data, size_t size, uint32_t timeoutMs = 1000);

    // Send a HID feature report (SET_REPORT Feature type via EP0 control pipe).
    // data[0] must be the feature report ID. Padded to FeatureReportByteLength automatically.
    // This is the command channel the original Steam Controller used for all firmware commands.
    bool SendFeatureReport(const uint8_t* data, size_t size);

    ULONG OutputReportByteLength()  const { return m_outputReportLen; }
    ULONG FeatureReportByteLength() const { return m_featureReportLen; }

    // Read the next HID input report. buffer[0] will be the report ID on return.
    // Returns bytes read, or 0 on timeout/error.
    size_t ReadInputReport(uint8_t* buffer, size_t size, uint32_t timeoutMs = 1000);

private:
    std::wstring m_path;      // retained so Reopen can change the share mode
    HANDLE m_handle           = INVALID_HANDLE_VALUE;
    HANDLE m_event            = INVALID_HANDLE_VALUE;
    ULONG  m_outputReportLen  = 64;
    ULONG  m_featureReportLen = 64;
};

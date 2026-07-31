#include "DeviceIdentity.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
// initguid.h must precede devpkey.h: without INITGUID the DEVPKEY_* keys are
// only extern declarations and there is no import library that supplies them,
// so this translation unit defines them. It is the only TU that includes
// devpkey.h, so nothing can collide.
#include <initguid.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <hidsdi.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <vector>

namespace {

// Formatted by hand rather than with StringFromGUID2, which would drag an ole32
// dependency into the SteamController library — and SteamProbe links that
// library without ole32.
std::wstring GuidToString(const GUID& guid) {
    wchar_t buffer[48] = {};
    swprintf_s(buffer, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
               guid.Data1, guid.Data2, guid.Data3,
               guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
               guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return buffer;
}

// Instance ID of the devnode that owns a device interface. Two-call pattern:
// the first call reports the required size, the second fills the buffer.
std::wstring GetInterfaceInstanceId(const std::wstring& devicePath) {
    DEVPROPTYPE type = 0;
    ULONG size = 0;
    CONFIGRET cr = CM_Get_Device_Interface_PropertyW(
        devicePath.c_str(), &DEVPKEY_Device_InstanceId, &type, nullptr, &size, 0);
    if (cr != CR_BUFFER_SMALL || size == 0)
        return {};

    std::vector<BYTE> buffer(size);
    cr = CM_Get_Device_Interface_PropertyW(
        devicePath.c_str(), &DEVPKEY_Device_InstanceId, &type, buffer.data(), &size, 0);
    if (cr != CR_SUCCESS || type != DEVPROP_TYPE_STRING)
        return {};

    // The property is a NUL-terminated wide string in the byte buffer.
    return std::wstring(reinterpret_cast<const wchar_t*>(buffer.data()));
}

}  // namespace

namespace DeviceIdentity {

std::wstring GetContainerId(const std::wstring& devicePath) {
    std::wstring instanceId = GetInterfaceInstanceId(devicePath);
    if (instanceId.empty())
        return {};

    DEVINST devInst = 0;
    if (CM_Locate_DevNodeW(&devInst, const_cast<DEVINSTID_W>(instanceId.c_str()),
                           CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
        return {};

    GUID container{};
    DEVPROPTYPE type = 0;
    ULONG size = sizeof(container);
    if (CM_Get_DevNode_PropertyW(devInst, &DEVPKEY_Device_ContainerId, &type,
                                 reinterpret_cast<PBYTE>(&container), &size, 0) != CR_SUCCESS)
        return {};
    if (type != DEVPROP_TYPE_GUID)
        return {};

    return GuidToString(container);
}

std::wstring GetSerialNumber(const std::wstring& devicePath) {
    // Opened with no access rights, purely to query descriptors — this must not
    // disturb a handle another process (or one of our own slots) already holds.
    HANDLE handle = CreateFileW(devicePath.c_str(), 0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return {};

    wchar_t serial[256] = {};
    const BOOLEAN ok = HidD_GetSerialNumberString(handle, serial, sizeof(serial));
    CloseHandle(handle);
    if (!ok)
        return {};

    serial[std::size(serial) - 1] = L'\0';
    return serial;
}

std::wstring GetPhysicalDeviceKey(const std::wstring& devicePath) {
    std::wstring containerId = GetContainerId(devicePath);
    if (!containerId.empty())
        return L"container:" + containerId;

    std::wstring serial = GetSerialNumber(devicePath);
    if (!serial.empty())
        return L"serial:" + serial;

    // Last resort: the path itself. Device paths are case-insensitive, so
    // normalise before comparing — an interface that comes back with different
    // casing after a device cycle must not read as a second device.
    std::wstring lowered = devicePath;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return L"path:" + lowered;
}

}  // namespace DeviceIdentity

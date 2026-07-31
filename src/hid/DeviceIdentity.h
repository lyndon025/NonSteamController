#pragma once
#include <string>

// Stable identity for a HID interface, so a controller can be recognised as the
// same one across reconnects (for per-controller settings, and for pairing a
// slot to its SDL gamepad).
//
// Read this before using it to deduplicate — it is the obvious use and it is
// wrong here. The wireless dongle presents its pairing slots as several HID
// interfaces of a *single* USB device, so all of them share one container ID.
// Collapsing on that would reduce a four-slot dongle to one controller.
//
// What actually separates a real controller from an idle interface is liveness:
// SteamController::Open probes for a state-shaped report and refuses the
// interface if none arrives. That is also the missing piece behind upstream
// SteamlessController's duplicate-Xbox-pad behaviour — it creates one virtual
// pad per enumerated path with no probe at all.
namespace DeviceIdentity {

// Container ID of the physical device behind a HID interface path, as a GUID
// string. Empty when unavailable — callers should then fall back to the serial
// number and finally to the path itself.
std::wstring GetContainerId(const std::wstring& devicePath);

// USB serial number string reported by the device, or empty.
//
// For the wireless dongle this is the *dongle's* serial, not the paired
// controller's — a controller's own identity is only visible through the vendor
// protocol. So serials cannot tell you that a wired controller and a dongle slot
// are the same physical pad either. In practice that case resolves itself: a
// controller talking over USB stops reporting through the dongle, so the idle
// dongle slot produces no state report and Open() skips it.
std::wstring GetSerialNumber(const std::wstring& devicePath);

// Identity key for the device behind an interface: container ID when available,
// else the serial, else the lowercased path. Prefixed with its source so keys
// from different sources can never collide.
//
// Note this identifies the *physical device*, which for a dongle means the
// dongle rather than any one paired controller. See the header comment above.
std::wstring GetPhysicalDeviceKey(const std::wstring& devicePath);

}  // namespace DeviceIdentity

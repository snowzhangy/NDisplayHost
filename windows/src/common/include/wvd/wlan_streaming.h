#pragma once
// Keeps the PC's Wi-Fi radio on its channel while a headset streams.
//
// Windows periodically background-scans on a connected Wi-Fi interface
// (roaming, location, network list). A scan takes the radio off-channel for a
// moment. With direct connect the same single radio also hosts the headset's
// network, so every scan drops video for ~1-2 s: measured on an MT7925 as a
// loss burst (10-30%, RTT flat) every 12-15 s.
//
// WlanSetInterface lets an unelevated process turn on media streaming mode
// and turn off background scanning. Both are tied to the WLAN client handle:
// Windows reverts them when the handle closes, and this class also restores
// the previous values explicitly.
#include "wvd/platform.h"

#include <wlanapi.h>

#include <vector>

#pragma comment(lib, "wlanapi.lib")

namespace wvd {

class WlanStreamingMode {
 public:
  WlanStreamingMode() = default;
  WlanStreamingMode(const WlanStreamingMode&) = delete;
  WlanStreamingMode& operator=(const WlanStreamingMode&) = delete;
  ~WlanStreamingMode() { Release(); }

  // Returns the number of interfaces that took at least one setting.
  int Engage() {
    if (client_) return static_cast<int>(saved_.size());
    DWORD version = 0;
    if (WlanOpenHandle(2, nullptr, &version, &client_) != ERROR_SUCCESS) { client_ = nullptr; return 0; }
    PWLAN_INTERFACE_INFO_LIST list = nullptr;
    if (WlanEnumInterfaces(client_, nullptr, &list) != ERROR_SUCCESS) return 0;
    for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
      Saved saved{list->InterfaceInfo[i].InterfaceGuid};
      saved.scanChanged = Set(saved.guid, wlan_intf_opcode_background_scan_enabled, FALSE, saved.scan);
      saved.streamChanged = Set(saved.guid, wlan_intf_opcode_media_streaming_mode, TRUE, saved.stream);
      if (saved.scanChanged || saved.streamChanged) saved_.push_back(saved);
    }
    WlanFreeMemory(list);
    return static_cast<int>(saved_.size());
  }

  void Release() {
    if (!client_) return;
    for (const auto& saved : saved_) {
      BOOL scan = saved.scan, stream = saved.stream;
      if (saved.scanChanged)
        WlanSetInterface(client_, &saved.guid, wlan_intf_opcode_background_scan_enabled, sizeof(BOOL), &scan, nullptr);
      if (saved.streamChanged)
        WlanSetInterface(client_, &saved.guid, wlan_intf_opcode_media_streaming_mode, sizeof(BOOL), &stream, nullptr);
    }
    saved_.clear();
    WlanCloseHandle(client_, nullptr);
    client_ = nullptr;
  }

 private:
  struct Saved {
    GUID guid;
    BOOL scan = TRUE, stream = FALSE;
    bool scanChanged = false, streamChanged = false;
  };

  // Records the current value in `previous` and applies `value` if it differs.
  bool Set(const GUID& guid, WLAN_INTF_OPCODE opcode, BOOL value, BOOL& previous) {
    DWORD size = 0;
    PBOOL current = nullptr;
    if (WlanQueryInterface(client_, &guid, opcode, nullptr, &size, reinterpret_cast<PVOID*>(&current), nullptr) !=
        ERROR_SUCCESS) return false;
    previous = *current ? TRUE : FALSE;
    WlanFreeMemory(current);
    if (previous == value) return false;
    return WlanSetInterface(client_, &guid, opcode, sizeof(BOOL), &value, nullptr) == ERROR_SUCCESS;
  }

  HANDLE client_ = nullptr;
  std::vector<Saved> saved_;
};

}  // namespace wvd

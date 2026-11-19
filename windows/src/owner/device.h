// Device owner (§4.1 process roles).
//
// Small, stable, no network, no media: holds the SwDevice handle for the
// IddCx device, applies topology, restores it if the driver host or this
// process restarts. Lifetime policy: P0 keeps the default
// SwDeviceLifetimeHandle — a dead owner never leaves orphaned monitors;
// SCM restarts the owner, which re-creates the device and restores
// topology.
#pragma once

#include <string>

#include "wvd/platform.h"  // windows.h first
#include "swdevice.h"

namespace wvd {

class DeviceOwner {
 public:
  DeviceOwner() = default;
  ~DeviceOwner() { Stop(); }

  // Creates (or re-creates) the software device the UMDF IDD binds to
  // (INF: root-enumerated, compatible ID ROOT\WVDIDD).
  bool Start(std::wstring& err);
  void Stop();
  bool created() const { return created_; }
  const std::wstring& instanceId() const { return instanceId_; }

 private:
  static void WINAPI CreateCallback(HSWDEVICE hSwDevice, HRESULT hr, PVOID ctx,
                                    PCWSTR instanceId);

  HSWDEVICE handle_ = nullptr;
  HANDLE doneEvent_ = nullptr;
  HRESULT createResult_ = E_FAIL;
  std::wstring instanceId_;
  bool created_ = false;
};

}  // namespace wvd

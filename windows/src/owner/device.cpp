#include "device.h"

#include <initguid.h>

#include "wvd/logging.h"

#pragma comment(lib, "swdevice.lib")

namespace wvd {

// Device interface class we register so other components (tray, media
// worker) can locate the WVD device via SetupDiGetClassDevs.
// {8A6B93E1-3B6A-4C9E-9E4E-4A53D21A61A0}
DEFINE_GUID(GUID_WVD_INTERFACE, 0x8a6b93e1, 0x3b6a, 0x4c9e, 0x9e, 0x4e, 0x4a, 0x53,
            0xd2, 0x1a, 0x61, 0xa0);

void WINAPI DeviceOwner::CreateCallback(HSWDEVICE, HRESULT hr, PVOID ctx,
                                        PCWSTR instanceId) {
  auto* self = static_cast<DeviceOwner*>(ctx);
  self->createResult_ = hr;
  if (instanceId) self->instanceId_ = instanceId;
  // Signal on FAILURE too: waiting the full timeout just to learn that the
  // device could not be created makes every failed start take 10 s.
  SetEvent(self->doneEvent_);
}

bool DeviceOwner::Start(std::wstring& err) {
  if (handle_) return true;
  doneEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);

  SW_DEVICE_CREATE_INFO info{};
  info.cbSize = sizeof(info);

  // These IDs must match the install sections in driver/wvdidd.inf, otherwise
  // PnP never binds the UMDF driver to the software device. pszzHardwareIds in
  // particular was missing, so nothing could match the INF.
  static const wchar_t kIds[] = L"WVDIDD\0\0";
  static const wchar_t kCompatIds[] = L"WVDIDD\0\0";
  info.pszInstanceId = L"WvdIdd";
  info.pszzHardwareIds = kIds;
  info.pszzCompatibleIds = kCompatIds;
  info.CapabilityFlags = SWDeviceCapabilitiesRemovable |
                         SWDeviceCapabilitiesSilentInstall |
                         SWDeviceCapabilitiesDriverRequired;
  info.pszDeviceDescription = L"WVD Virtual Display Adapter";

  // Parent = the root-enumerated node. An empty parent is invalid and the
  // create fails; Root\WVDIDD (below) is a root-enumerated hardware ID, so the
  // parent must be HTREE\ROOT\0.
  HRESULT hr = SwDeviceCreate(L"WVDIDD", L"HTREE\\ROOT\\0", &info, 0, nullptr,
                              &CreateCallback, this, &handle_);
  if (FAILED(hr)) {
    wchar_t buf[160];
    _snwprintf_s(buf, _TRUNCATE,
                 L"SwDeviceCreate hr=0x%08X (GLE=%lu)", hr, GetLastError());
    err = buf;
    WVD_LOGE("%ls — is the IDD driver package installed? (driver/ + scripts)", err.c_str());
    handle_ = nullptr;
    return false;
  }

  // Wait for the create callback (device enumeration can be slow at boot). The
  // callback now signals on failure as well, so this only blocks on real work.
  DWORD w = WaitForSingleObject(doneEvent_, 10000);
  if (w != WAIT_OBJECT_0) {
    err = L"SwDeviceCreate callback timed out";
    WVD_LOGW("%ls", err.c_str());
    // Keep the handle: enumeration may still complete (test-sign lab cases).
  } else if (FAILED(createResult_)) {
    wchar_t buf[160];
    _snwprintf_s(buf, _TRUNCATE, L"device create failed in callback hr=0x%08X", createResult_);
    err = buf;
    WVD_LOGE("%ls (driver installed? test signing on?)", err.c_str());
    SwDeviceClose(handle_);
    handle_ = nullptr;
    return false;
  }

  // Lifetime policy (§4.1): P0 explicitly keeps SwDeviceLifetimeHandle
  // (the API default) so a dead owner takes the device with it and no
  // orphaned monitors can linger. SwDeviceLifetimeParentPresent is
  // evaluated post-P0 with reclaim + uninstall-safety work.
  SW_DEVICE_LIFETIME lt = SWDeviceLifetimeMax;
  if (SUCCEEDED(SwDeviceGetLifetime(handle_, &lt)) && lt != SWDeviceLifetimeHandle) {
    WVD_LOGW("lifetime was %d — forcing SwDeviceLifetimeHandle (§4.1 P0 policy)",
             int(lt));
    SwDeviceSetLifetime(handle_, SWDeviceLifetimeHandle);
  }

  created_ = SUCCEEDED(createResult_);
  if (created_)
    WVD_LOGI("software device created: %ls (lifetime=handle)", instanceId_.c_str());
  else {
    wchar_t buf[128];
    _snwprintf_s(buf, _TRUNCATE, L"create callback hr=0x%08X", createResult_);
    err = buf;
  }
  return created_;
}

void DeviceOwner::Stop() {
  if (handle_) {
    SwDeviceClose(handle_);  // lifetime=handle → device departs (§4.1)
    handle_ = nullptr;
    created_ = false;
    WVD_LOGI("software device closed (monitors depart with the handle)");
  }
  if (doneEvent_) {
    CloseHandle(doneEvent_);
    doneEvent_ = nullptr;
  }
}

}  // namespace wvd

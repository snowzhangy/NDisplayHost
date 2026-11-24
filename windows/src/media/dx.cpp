#include "dx.h"

#include <d3d11_4.h>

#include "wvd/logging.h"

namespace wvd {

void EnableD3D11ThreadSafety(ID3D11Device* device) {
  if (!device) return;
  ComPtr<ID3D11Multithread> mt;
  if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&mt)))) {
    mt->SetMultithreadProtected(TRUE);
  } else {
    WVD_LOGW("ID3D11Multithread unavailable — capture/encode share a context unsafely");
  }
}


void RaiseGpuPriority(ID3D11Device* device) {
  // Capture/convert work is tiny but latency-critical; without this a game
  // that saturates the GPU can starve it for whole frames. Best effort: the
  // call is refused for processes that may not raise priority. This is only
  // the device's rank INSIDE the process's scheduling class — see
  // RaiseProcessPriority for the class itself.
  ComPtr<IDXGIDevice> dxgi;
  if (device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi)))) {
    if (FAILED(dxgi->SetGPUThreadPriority(7)) && FAILED(dxgi->SetGPUThreadPriority(2)))
      WVD_LOGW("GPU thread priority could not be raised");
  }
}

void RaiseProcessPriority() {
  // A full-screen game keeps the GPU busy at NORMAL scheduling class, the
  // same class this process gets by default, so the desktop copy and NV12
  // convert queue behind whole game frames. Each late picture misses its
  // deadline and costs a keyframe. A HIGH GPU class schedules them ahead of
  // the game. That is a few hundred microseconds of GPU time per picture, so
  // the game barely notices. HIGH rather than REALTIME: REALTIME can also
  // preempt the compositor and is known to freeze some drivers' encoders
  // with hardware-accelerated GPU scheduling.
  // The class covers every D3D device of the process, including the ones a
  // Media Foundation encoder creates for itself. Neither call needs elevation.
  using SetSchedulingClass = LONG(APIENTRY*)(HANDLE, INT);
  constexpr INT kGpuClassHigh = 4;  // D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH
  const HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
  const auto setClass = gdi ? reinterpret_cast<SetSchedulingClass>(GetProcAddress(
                                  gdi, "D3DKMTSetProcessSchedulingPriorityClass"))
                            : nullptr;
  const LONG gpuStatus = setClass ? setClass(GetCurrentProcess(), kGpuClassHigh) : -1;
  // The network threads (QUIC acks, pongs, the datagram pacer) and the
  // capture/encode threads mostly sleep; HIGH keeps the game's busy threads
  // from delaying them.
  const bool cpuHigh = SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS) != FALSE;
  if (gpuStatus == 0 && cpuHigh)
    WVD_LOGI("priority: GPU scheduling class high, CPU priority class high");
  else
    WVD_LOGW("priority: GPU scheduling class %s (status 0x%08lx), CPU priority class %s",
             gpuStatus == 0 ? "high" : "unchanged", static_cast<unsigned long>(gpuStatus),
             cpuHigh ? "high" : "unchanged");
}

ComPtr<IDXGIFactory1> CreateDxgiFactory() {
  ComPtr<IDXGIFactory1> f;
  HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&f));
  if (FAILED(hr)) {
    WVD_LOGE("CreateDXGIFactory1 failed hr=%s", HresultStr(hr).c_str());
    return nullptr;
  }
  return f;
}

std::vector<DxAdapterInfo> EnumerateAdapters(IDXGIFactory1* factory) {
  std::vector<DxAdapterInfo> out;
  if (!factory) return out;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> a;
    if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 d{};
    a->GetDesc1(&d);
    DxAdapterInfo info;
    info.index = i;
    info.name = d.Description;
    info.luid = d.AdapterLuid;
    info.isSoftware = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    info.vramMB = d.DedicatedVideoMemory / (1024 * 1024);
    ComPtr<IDXGIAdapter3> a3;
    if (SUCCEEDED(a.As(&a3))) {
      DXGI_QUERY_VIDEO_MEMORY_INFO q{};
      if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &q)))
        info.vramMB = q.Budget / (1024 * 1024);
    }
    out.push_back(info);
  }
  return out;
}

std::vector<DxOutputInfo> EnumerateOutputs(IDXGIFactory1* factory) {
  std::vector<DxOutputInfo> out;
  if (!factory) return out;
  for (UINT ai = 0;; ++ai) {
    ComPtr<IDXGIAdapter1> a;
    if (factory->EnumAdapters1(ai, &a) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 ad{};
    a->GetDesc1(&ad);
    if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
    for (UINT oi = 0;; ++oi) {
      ComPtr<IDXGIOutput> o;
      if (a->EnumOutputs(oi, &o) == DXGI_ERROR_NOT_FOUND) break;
      DXGI_OUTPUT_DESC od{};
      if (FAILED(o->GetDesc(&od)) || !od.AttachedToDesktop ||
          od.DesktopCoordinates.right <= od.DesktopCoordinates.left ||
          od.DesktopCoordinates.bottom <= od.DesktopCoordinates.top) continue;
      DxOutputInfo info;
      info.adapterIndex = ai;
      info.monitorName = od.DeviceName;
      info.desktopCoords = od.DesktopCoordinates;
      info.adapterLuid = ad.AdapterLuid;
      DISPLAY_DEVICEW monitor{};
      monitor.cb = sizeof(monitor);
      if (EnumDisplayDevicesW(od.DeviceName, 0, &monitor, EDD_GET_DEVICE_INTERFACE_NAME))
        info.monitorDevicePath = monitor.DeviceID;
      DXGI_MODE_DESC want{}, m{};
      if (SUCCEEDED(o->FindClosestMatchingMode(&want, &m, nullptr))) {
        info.modeWidth = m.Width;
        info.modeHeight = m.Height;
        if (m.RefreshRate.Denominator)
          info.modeRateHz = m.RefreshRate.Numerator / m.RefreshRate.Denominator;
      }
      out.push_back(info);
    }
  }
  return out;
}

HRESULT CreateDeviceForAdapter(IDXGIFactory1* factory, const LUID& luid,
                               ComPtr<ID3D11Device>& device,
                               ComPtr<ID3D11DeviceContext>& ctx) {
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> a;
    if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 d{};
    a->GetDesc1(&d);
    if (d.AdapterLuid.HighPart == luid.HighPart && d.AdapterLuid.LowPart == luid.LowPart) {
      adapter = a;
      break;
    }
  }
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
  flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
  D3D_FEATURE_LEVEL fl{};
  HRESULT hr = D3D11CreateDevice(adapter.Get(),
                                 adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                 nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &device, &fl,
                                 &ctx);
  if (FAILED(hr)) {
    // Retry without debug layer (SDK not installed on many machines).
    hr = D3D11CreateDevice(adapter.Get(),
                           adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                           nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                           D3D11_SDK_VERSION, &device, &fl, &ctx);
  }
  if (SUCCEEDED(hr)) {
    EnableD3D11ThreadSafety(device.Get());
    RaiseGpuPriority(device.Get());
    WVD_LOGI("D3D11 device created (FL %d.%d, luid %lu:%lu)", int(fl >> 12),
             int((fl >> 8) & 0xF), luid.HighPart, luid.LowPart);
  }
  return hr;
}

ComPtr<IDXGIOutput> FindDxgiOutput(IDXGIFactory1* factory, const LUID& luid,
                                   const std::wstring& deviceName) {
  if (!factory) return nullptr;
  for (UINT ai = 0;; ++ai) {
    ComPtr<IDXGIAdapter1> a;
    if (factory->EnumAdapters1(ai, &a) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 ad{};
    a->GetDesc1(&ad);
    if (ad.AdapterLuid.HighPart != luid.HighPart || ad.AdapterLuid.LowPart != luid.LowPart)
      continue;
    for (UINT ox = 0;; ++ox) {
      ComPtr<IDXGIOutput> o;
      if (a->EnumOutputs(ox, &o) == DXGI_ERROR_NOT_FOUND) break;
      DXGI_OUTPUT_DESC od{};
      o->GetDesc(&od);
      if (_wcsicmp(od.DeviceName, deviceName.c_str()) == 0) return o;
    }
  }
  return nullptr;
}

bool FindOutput(IDXGIFactory1* factory, const wchar_t* monitorName, DxOutputInfo& out) {
  return SelectOutput(EnumerateOutputs(factory), monitorName, out);
}

bool SelectOutput(const std::vector<DxOutputInfo>& outputs, const wchar_t* monitorName,
                  DxOutputInfo& out) {
  if (outputs.empty()) return false;
  if (!monitorName || !*monitorName) {
    // Primary = the output containing (0,0).
    for (auto& o : outputs)
      if (o.desktopCoords.left == 0 && o.desktopCoords.top == 0) {
        out = o;
        return true;
      }
    out = outputs[0];
    return true;
  }
  for (auto& o : outputs) {
    const wchar_t* shortName = wcsrchr(o.monitorName.c_str(), L'\\');
    if (_wcsicmp(o.monitorName.c_str(), monitorName) == 0 ||
        (!o.monitorDevicePath.empty() && _wcsicmp(o.monitorDevicePath.c_str(), monitorName) == 0) ||
        (shortName && _wcsicmp(shortName + 1, monitorName) == 0)) {
      out = o;
      return true;
    }
  }
  return false;
}

}  // namespace wvd

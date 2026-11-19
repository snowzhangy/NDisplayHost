#include "d3d12ve_probe.h"

#include <d3d12.h>
#include <d3d12video.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "wvd/logging.h"

#pragma comment(lib, "d3d12.lib")

namespace wvd {

using Microsoft::WRL::ComPtr;

D3d12VeProbeResult ProbeD3D12VideoEncode(const LUID& adapterLuid) {
  D3d12VeProbeResult r;
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    r.detail = "CreateDXGIFactory1 failed";
    return r;
  }
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> a;
    if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 d{};
    a->GetDesc1(&d);
    if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
    if (d.AdapterLuid.HighPart == adapterLuid.HighPart &&
        d.AdapterLuid.LowPart == adapterLuid.LowPart) {
      adapter = a;
      r.adapterName = d.Description;
      break;
    }
  }

  ComPtr<ID3D12Device> dev;
  HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&dev));
  if (FAILED(hr)) {
    r.detail = "D3D12CreateDevice failed hr=" + HresultStr(hr);
    return r;
  }
  r.deviceOk = true;

  ComPtr<ID3D12VideoDevice> vdev;
  hr = dev.As(&vdev);
  if (FAILED(hr)) {
    r.detail = "no ID3D12VideoDevice (pre-Win11 driver?)";
    return r;
  }

  auto codecSupported = [&](D3D12_VIDEO_ENCODER_CODEC c) {
    D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC f{};
    f.NodeIndex = 0;
    f.Codec = c;
    if (FAILED(vdev->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_CODEC, &f,
                                         sizeof(f))))
      return false;
    return f.IsSupported != FALSE;
  };

  r.h264 = codecSupported(D3D12_VIDEO_ENCODER_CODEC_H264);
  r.hevc = codecSupported(D3D12_VIDEO_ENCODER_CODEC_HEVC);
#if defined(D3D12_VIDEO_ENCODER_CODEC_AV1)
  r.av1 = codecSupported(D3D12_VIDEO_ENCODER_CODEC_AV1);
#endif

  // Largest supported encode resolution (§4.6 max-frame-size row).
  {
    D3D12_FEATURE_DATA_VIDEO_ENCODER_OUTPUT_RESOLUTION f{};
    f.NodeIndex = 0;
    f.Codec = r.hevc ? D3D12_VIDEO_ENCODER_CODEC_HEVC
                     : D3D12_VIDEO_ENCODER_CODEC_H264;
    f.ResolutionRatiosCount = 0;
    f.pResolutionRatios = nullptr;
    if (SUCCEEDED(vdev->CheckFeatureSupport(
            D3D12_FEATURE_VIDEO_ENCODER_OUTPUT_RESOLUTION, &f, sizeof(f))) &&
        f.IsSupported) {
      r.maxWidth = f.MaxResolutionSupported.Width;
      r.maxHeight = f.MaxResolutionSupported.Height;
    }
  }
  r.detail = "probe ok";
  return r;
}

}  // namespace wvd

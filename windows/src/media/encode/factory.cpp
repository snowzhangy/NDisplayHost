#include "iencoder.h"

#include <algorithm>

#include "d3d12ve_probe.h"
#include "mf_encoder.h"
#include "nvenc_encoder.h"
#include "null_encoder.h"
#include "wvd/logging.h"

namespace wvd {

// Declared in vendor_stubs.cpp.
bool AmfRuntimeAvailable();
std::unique_ptr<IVideoEncoder> CreateAmfEncoder(const EncoderInitDesc&);

std::vector<EncoderCandidate> ProbeEncoderCandidates(const EncoderInitDesc& desc) {
  std::vector<EncoderCandidate> v;

  // Tier 1 — NVIDIA NVENC.
  {
    EncoderCandidate c;
    c.vendor = Vendor::NVIDIA;
    c.tier = 1;
    c.name = "nvenc-hevc";
    if (!NvencModuleBuilt()) {
      c.available = false;
      c.reason = "module not compiled (set WVD_NVENC_SDK_DIR)";
    } else if (!NvencRuntimeAvailable()) {
      c.available = false;
      c.reason = "nvEncodeAPI64.dll missing / no NVIDIA driver";
    } else {
      c.available = true;
      c.caps.hevc8 = true;
      c.reason = "API runtime present; HEVC session/size checked at creation";
      c.create = CreateNvencEncoder;
    }
    v.push_back(std::move(c));
  }

  // Tier 1 — AMD AMF.
  {
    EncoderCandidate c;
    c.vendor = Vendor::AMD;
    c.tier = 1;
    c.name = "amf-hevc";
    c.available = false;
    c.reason = AmfRuntimeAvailable()
                   ? "runtime present; encoder module pending (Phase 0b)"
                   : "amfrt64.dll missing";
    c.create = CreateAmfEncoder;
    v.push_back(std::move(c));
  }

  // Tier 2 — D3D12 Video Encode (vendor-neutral probe, §4.6).
  {
    EncoderCandidate c;
    c.vendor = Vendor::Microsoft;
    c.tier = 2;
    c.name = "d3d12ve";
    auto probe = ProbeD3D12VideoEncode(desc.adapterLuid);
    char det[192];
    _snprintf_s(det, sizeof(det), _TRUNCATE,
                "probe: hevc=%d h264=%d av1=%d max=%ux%u", probe.hevc ? 1 : 0,
                probe.h264 ? 1 : 0, probe.av1 ? 1 : 0, probe.maxWidth,
                probe.maxHeight);
    c.caps.hevc8 = probe.hevc;
    c.caps.h264 = false;  // (caps struct keeps hevc; h264 noted in reason)
    c.caps.av1 = probe.av1;
    c.caps.maxFrameWidth = probe.maxWidth;
    c.caps.maxFrameHeight = probe.maxHeight;
    c.available = false;  // encode implementation lands Phase 1
    c.reason = det;
    v.push_back(std::move(c));
  }

  // Tier 3 — the GPU vendor's hardware MFT through Media Foundation. On AMD
  // and Intel this is the working path until the native AMF/QSV modules land.
  {
    EncoderCandidate c;
    c.vendor = Vendor::Microsoft;
    c.tier = 3;
    c.name = "mf-hw";
    auto mf = ProbeMfEncoders(desc.adapterLuid);
    if (mf.hwHevc || mf.hwH264) {
      c.available = true;
      c.reason = "'" + mf.hwName + "' (" + (mf.hwHevc ? "HEVC" : "") +
                 (mf.hwHevc && mf.hwH264 ? "+" : "") + (mf.hwH264 ? "H.264" : "") + ")";
      c.caps.hevc8 = mf.hwHevc;
      c.caps.h264 = mf.hwH264;
      c.caps.maxFrameWidth = 8192;
      c.caps.maxFrameHeight = 4320;
      c.create = CreateMfHardwareEncoder;
    } else {
      c.reason = "no hardware encoder MFT for this adapter";
    }
    v.push_back(std::move(c));

    // Tier 4 — software H.264 (Microsoft MFT). Last real encoder: slow, but
    // the headset can still decode it.
    EncoderCandidate s;
    s.vendor = Vendor::Microsoft;
    s.tier = 4;
    s.name = "mf-sw-h264";
    s.available = mf.swH264;
    s.reason = mf.swH264 ? "Microsoft H.264 software MFT" : "software H.264 MFT missing";
    s.caps.h264 = mf.swH264;
    s.caps.maxFrameWidth = 4096;
    s.caps.maxFrameHeight = 2304;
    if (mf.swH264) s.create = CreateMfSoftwareEncoder;
    v.push_back(std::move(s));
  }

  // Tier 4 — test encoder (opt-in; never chosen for production use).
  {
    EncoderCandidate c;
    c.vendor = Vendor::Test;
    c.tier = 4;
    c.name = "null-test";
    c.available = true;
    c.reason = "pipeline test encoder (opt-in via --codec null)";
    c.caps.hevc8 = true;
    c.caps.intraRefresh = true;
    c.create = [](const EncoderInitDesc& d) -> std::unique_ptr<IVideoEncoder> {
      return std::make_unique<NullEncoder>(d);
    };
    v.push_back(std::move(c));
  }

  return v;
}

std::unique_ptr<IVideoEncoder> CreateBestEncoder(const EncoderInitDesc& desc,
                                                 std::string* outName) {
  // The null test encoder is opt-in only (P0 harness): it emits pseudo-AUs.
  if (desc.pinEncoder == "null-test" || desc.tuningProfile == "null") {
    auto e = std::make_unique<NullEncoder>(desc);
    if (outName) *outName = "null-test";
    return e;
  }
  for (auto& c : ProbeEncoderCandidates(desc)) {
    if (!desc.pinEncoder.empty() && c.name != desc.pinEncoder) continue;
    if (!c.available || !c.create) continue;
    if (desc.hevcOnly && !c.caps.hevc8) continue;
    if (c.vendor == Vendor::Test) continue;  // never automatic
    auto e = c.create(desc);
    if (e) {
      if (outName) *outName = c.name;
      WVD_LOGI("encoder selected: %s (tier %u)", c.name.c_str(), c.tier);
      if (e->tier() >= 4)
        WVD_LOGW("software encoder in use — expect high CPU and latency (§4.6 tier 4)");
      return e;
    }
    WVD_LOGW("encoder %s selected but init failed — next tier (§4.6)",
             c.name.c_str());
  }
  if (!desc.pinEncoder.empty())
    WVD_LOGE("requested encoder '%s' is not available here (ndisplay diag caps)",
             desc.pinEncoder.c_str());
  return nullptr;
}

std::unique_ptr<IVideoEncoder> CreateNextTier(const std::string& failedName,
                                              const EncoderInitDesc& desc,
                                              std::string* outName) {
  bool past = false;
  for (auto& c : ProbeEncoderCandidates(desc)) {
    if (past) {
      if (!c.available || !c.create) continue;
      if (desc.hevcOnly && !c.caps.hevc8) continue;
      if (c.vendor == Vendor::Test && desc.tuningProfile != "null") continue;
      auto e = c.create(desc);
      if (e) {
        if (outName) *outName = c.name;
        WVD_LOGW("HOT-SWITCH encoder %s -> %s (§4.6: new IDR required)",
                 failedName.c_str(), c.name.c_str());
        return e;
      }
    }
    if (c.name == failedName) past = true;
  }
  WVD_LOGE("no lower tier available after %s", failedName.c_str());
  return nullptr;
}

}  // namespace wvd

// Encoder abstraction — "one vendor-neutral pipeline; only the encoder
// module is vendor-specific" (§4.6).
//
// Tiers: 1 = NVENC / AMF, 2 = D3D12 Video Encode, 3 = MF hardware MFT,
// 4 = software. The factory probes capabilities and selects; the session
// hot-switches to the next tier on encoder error (§4.6 degradation rules).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "wvd/platform.h"
#include "wvd/protocol.h"

struct ID3D11Device;
struct ID3D11Texture2D;

namespace wvd {

enum class Vendor : uint8_t { NVIDIA, AMD, Intel, Qualcomm, Microsoft, Test };

struct EncoderCaps {
  bool h264 = false;
  bool hevc8 = false;
  bool hevc10 = false;          // HDR, post-G1
  bool av1 = false;
  bool hevc444 = false;         // not a plan lever (§6.1) — probe only
  bool intraRefresh = false;
  bool ltr = false;             // long-term reference
  bool refInvalidation = false;
  bool qpMap = false;           // ROI / QP-delta / emphasis map
  bool emphasisMapNoAQ = false; // NVENC: emphasis map requires AQ off (§4.6)
  bool sliceOutput = false;     // sub-frame (slice) delivery
  uint32_t maxFrameWidth = 0;
  uint32_t maxFrameHeight = 0;
  uint32_t maxSessions = 1;
};

struct EncoderInitDesc {
  uint32_t width = 0, height = 0;
  uint32_t fpsNum = 60, fpsDen = 1;
  uint32_t bitrateKbps = 60000;
  Codec codec = Codec::HEVC;
  ColorVui vui = kSdrReferenceVui;  // §6.7; VUI writing verified per encoder
  LUID adapterLuid{};
  // The device the source pictures live on. With it a hardware encoder takes
  // the NV12 texture directly (no CPU round trip); null = CPU input only.
  ID3D11Device* device = nullptr;
  // Keyframes are sent on demand (first picture, recovery request, encoder
  // switch). 0 = no periodic keyframes: a periodic IDR is a multi-frame-sized
  // burst that a one-frame VBV turns into a visible quality pulse.
  uint32_t gopSeconds = 0;
  bool lowLatency = true;      // no B-frames, ~1 frame VBV, intra refresh
  bool preferQuality = true;
  bool hevcOnly = false;      // wide full stereo can exceed the H.264 encoder limit
  uint32_t hevcLevel = 0;     // HEVC general_level_idc, 0 = chosen by the encoder
  std::string tuningProfile;   // "N-AQ", "N-EMPH", "AMF-RDNA3"… (§4.6)
  // Pin one candidate by name ("mf-hw", "mf-sw-h264", "nvenc-hevc", "null-test");
  // empty = best available by tier.
  std::string pinEncoder;
};

// One source picture (always a full frame, §4.1). Hardware encoders use
// `texture`; anything else asks for the packed CPU copy, which is produced
// only on that request.
struct SourcePicture {
  // NV12 texture on EncoderInitDesc::device, or null. Its content stays
  // unchanged for the next three pictures.
  ID3D11Texture2D* texture = nullptr;
  const uint8_t* nv12 = nullptr;  // packed, size = Nv12Size(w,h); may be null
  size_t nv12Size = 0;
  // Fetches the packed CPU copy when `nv12` is null (GPU readback).
  std::function<HRESULT(const uint8_t** data, size_t* size)> readCpu;
  uint64_t contentVer = 0;
  int64_t qpcCapture = 0;
  uint64_t layoutGen = 0;
  uint32_t videoEpoch = 1;
  bool qualityBoost = false;      // refinement frame (low QP, §6.4)
  bool depthValid = false;        // embedded-depth strip in this picture is real

  HRESULT Cpu(const uint8_t** data, size_t* size) const {
    if (nv12 && nv12Size) {
      *data = nv12;
      *size = nv12Size;
      return S_OK;
    }
    return readCpu ? readCpu(data, size) : E_INVALIDARG;
  }
};

struct EncodedAu {
  std::vector<uint8_t> bytes;     // access unit (annex B)
  bool key = false;               // random access point (IDR/CRA/intra-done)
  // Identity of the SOURCE PICTURE these bytes encode. For a pipelined
  // encoder the output can lag the input, so these must describe the picture
  // the bytes came from, not the picture submitted by this call — the session
  // stamps packet deadlines/flags from them.
  uint64_t contentVer = 0;
  int64_t qpcCapture = 0;
  uint64_t layoutGen = 0;
  uint32_t videoEpoch = 1;
  bool refinement = false;        // that picture was a quiet-refinement frame
  bool depthValid = false;        // that picture carries a real DEPTH_VALID strip
  int64_t qpcEncodeIn = 0;
  int64_t qpcEncodeOut = 0;
};

class IVideoEncoder {
 public:
  virtual ~IVideoEncoder() = default;
  virtual const char* name() const = 0;
  virtual Vendor vendor() const = 0;
  virtual uint8_t tier() const = 0;  // 1..4 (§4.6)
  virtual const EncoderCaps& caps() const = 0;
  // Bitstream codec actually produced (goes into CodecConfig; the headset
  // builds its decoder from it). The null test encoder reports HEVC but emits
  // pseudo-AUs that no real decoder accepts.
  virtual Codec codec() const { return Codec::HEVC; }
  // Out-of-band parameter sets (Annex B) if the encoder publishes them before
  // the first picture; empty otherwise (they then arrive in the first keyframe).
  virtual std::vector<uint8_t> codecPrivate() const { return {}; }

  // Encode one picture. Returns S_OK and fills `au`, or HRESULT failure
  // (session then hot-switches tiers). qpcEncodeIn/Out are stamped here.
  // S_OK with an empty `au.bytes` means "no output for this input yet"
  // (pipelined encoder); the session then sends nothing for this picture.
  virtual HRESULT Encode(const SourcePicture& src, EncodedAu& au) = 0;

  // Next picture must be independently decodable (IDR or forced intra
  // refresh start) — §6.6 recovery.
  virtual HRESULT ForceKeyframe() = 0;

  // Change the target bitrate without restarting the stream (no keyframe).
  // E_NOTIMPL when the encoder cannot; the session then stops adapting.
  virtual HRESULT SetBitrate(uint32_t kbps) {
    (void)kbps;
    return E_NOTIMPL;
  }
  // True when pictures are consumed as GPU textures (reported in the log).
  virtual bool zeroCopy() const { return false; }
};

struct EncoderCandidate {
  Vendor vendor = Vendor::Test;
  uint8_t tier = 4;
  std::string name;
  bool available = false;
  std::string reason;      // why unavailable
  EncoderCaps caps;
  std::unique_ptr<IVideoEncoder> (*create)(const EncoderInitDesc&) = nullptr;
};

// Probe all tiers on this machine (§4.6 capability matrix). NVENC and AMF
// probe dynamically loads vendor runtimes; absent → marked unavailable.
std::vector<EncoderCandidate> ProbeEncoderCandidates(const EncoderInitDesc& desc);

// Create the best available encoder; `outName` receives the choice.
// On encoder failure mid-session the session calls CreateNextTier(name) to
// hot-switch without dropping the connection (§4.6).
std::unique_ptr<IVideoEncoder> CreateBestEncoder(const EncoderInitDesc& desc,
                                                 std::string* outName);
std::unique_ptr<IVideoEncoder> CreateNextTier(const std::string& failedName,
                                              const EncoderInitDesc& desc,
                                              std::string* outName);

}  // namespace wvd

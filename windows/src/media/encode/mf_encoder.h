// Media Foundation encoder module (§4.6 tiers 3 and 4).
//
//   mf-hw       Tier 3: the GPU vendor's hardware encoder MFT (AMD/Intel/
//               NVIDIA/Qualcomm all ship one with the driver). HEVC when the
//               MFT offers it, else H.264. Async MFT driven through its event
//               queue; low-latency mode, CBR, no B-frames.
//   mf-sw-h264  Tier 4: Microsoft's software H.264 MFT. Always present on
//               desktop Windows; slow, but keeps the pipeline real (decodable
//               by the headset) on machines with no usable hardware encoder.
//
// Input: a hardware MFT that is D3D11-aware shares the session's device
// (IMFDXGIDeviceManager) and takes the NV12 texture directly; otherwise, and
// for the software MFT, the packed NV12 CPU copy is used. Keyframes are on
// demand, and the target bitrate can be changed while streaming.
// Output is Annex B (start codes); parameter sets arrive in-band on keyframes
// and are also exposed through codecPrivate() when the MFT publishes them.
#pragma once

#include <memory>
#include <string>

#include "iencoder.h"

namespace wvd {

struct MfProbeResult {
  bool hwHevc = false;
  bool hwH264 = false;
  bool swH264 = false;
  std::string hwName;  // friendly name of the first hardware MFT
};

// adapterLuid narrows hardware MFTs to that GPU when non-zero.
MfProbeResult ProbeMfEncoders(const LUID& adapterLuid);

// H.264/HEVC CODECAPI_AVEncCommonBufferSize is in BYTES, not bits.
// Size a one-picture HRD buffer using the exact rational frame rate.
uint32_t MfFrameBufferBytes(uint32_t kbps, uint32_t fpsNum, uint32_t fpsDen);

std::unique_ptr<IVideoEncoder> CreateMfHardwareEncoder(const EncoderInitDesc& d);
std::unique_ptr<IVideoEncoder> CreateMfSoftwareEncoder(const EncoderInitDesc& d);

// Annex B helpers shared by the session (codec config) and diagnostics.
// Returns the concatenated parameter-set NAL units (with start codes) found in
// `au`: VPS/SPS/PPS for HEVC, SPS/PPS for H.264. Empty when there are none.
std::vector<uint8_t> ExtractParameterSets(Codec codec, const uint8_t* au, size_t len);
// True if `au` contains an IDR/IRAP slice.
bool ContainsKeyframe(Codec codec, const uint8_t* au, size_t len);

}  // namespace wvd

#include "null_encoder.h"

#include <cstring>

#include "wvd/time.h"

namespace wvd {

NullEncoder::NullEncoder(const EncoderInitDesc& d) : desc_(d) {
  caps_.hevc8 = true;  // stands in for the reference SDR profile
  caps_.intraRefresh = true;
  caps_.ltr = true;
  caps_.maxFrameWidth = 8192;
  caps_.maxFrameHeight = 4320;
}

HRESULT NullEncoder::Encode(const SourcePicture& src, EncodedAu& au) {
  const uint8_t* nv12 = nullptr;
  size_t nv12Size = 0;
  const HRESULT hr = src.Cpu(&nv12, &nv12Size);
  if (FAILED(hr)) return hr;
  if (nv12Size < size_t(desc_.width) * desc_.height) return E_INVALIDARG;
  au.qpcEncodeIn = QpcNow();

  const bool key = forceKey_ || (frames_ % 300) == 0;
  forceKey_ = false;
  ++frames_;

  // "Encoded" payload: magic + key flag + 16×9 luma thumbnail + FNV hash of
  // the full frame (lets the sink verify capture/convert correctness).
  constexpr int TW = 16, TH = 9;
  uint8_t thumb[TW * TH];
  const uint32_t W = desc_.width, H = desc_.height;
  for (int ty = 0; ty < TH; ++ty) {
    for (int tx = 0; tx < TW; ++tx) {
      uint32_t sx = uint32_t((tx + 0.5f) / TW * W);
      uint32_t sy = uint32_t((ty + 0.5f) / TH * H);
      sx = sx < W ? sx : W - 1;
      sy = sy < H ? sy : H - 1;
      thumb[ty * TW + tx] = nv12[size_t(sy) * W + sx];
    }
  }
  uint64_t h = 1469598103934665603ull;
  const uint8_t* p = nv12;
  for (size_t i = 0; i < nv12Size; i += 64) {  // strided sample
    h ^= p[i];
    h *= 1099511628211ull;
  }

  au.bytes.clear();
  au.bytes.push_back(key ? 'K' : 'D');  // pseudo NAL type
  au.bytes.insert(au.bytes.end(), thumb, thumb + sizeof(thumb));
  const uint8_t* hb = reinterpret_cast<const uint8_t*>(&h);
  au.bytes.insert(au.bytes.end(), hb, hb + 8);

  au.key = key;
  au.contentVer = src.contentVer;
  au.qpcCapture = src.qpcCapture;
  au.layoutGen = src.layoutGen;
  au.videoEpoch = src.videoEpoch;
  au.refinement = src.qualityBoost;
  au.depthValid = src.depthValid;
  au.qpcEncodeOut = QpcNow();
  return S_OK;
}

}  // namespace wvd

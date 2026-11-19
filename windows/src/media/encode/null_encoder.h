// Null/test encoder: keeps the pipeline, generations, deadlines and stats
// exercisable end-to-end on machines without a vendor SDK (and for CI).
// Produces tiny pseudo AUs carrying a 16×9 luma thumbnail + frame hash.
#pragma once

#include "iencoder.h"

namespace wvd {

class NullEncoder final : public IVideoEncoder {
 public:
  explicit NullEncoder(const EncoderInitDesc& d);
  const char* name() const override { return "null-test"; }
  Vendor vendor() const override { return Vendor::Test; }
  uint8_t tier() const override { return 4; }
  const EncoderCaps& caps() const override { return caps_; }
  HRESULT Encode(const SourcePicture& src, EncodedAu& au) override;
  HRESULT ForceKeyframe() override { forceKey_ = true; return S_OK; }

 private:
  EncoderInitDesc desc_;
  EncoderCaps caps_;
  bool forceKey_ = true;
  uint32_t frames_ = 0;
};

}  // namespace wvd

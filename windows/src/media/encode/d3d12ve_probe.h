// Tier-2 probe: D3D12 Video Encode capability query (§4.6).
// Queries codecs / frame sizes per adapter. The encode implementation module
// lands in Phase 1; the probe is real and used by the diag role and the factory.
#pragma once

#include <string>
#include <vector>

#include "iencoder.h"
#include "wvd/platform.h"

namespace wvd {

struct D3d12VeProbeResult {
  bool deviceOk = false;
  bool hevc = false;
  bool h264 = false;
  bool av1 = false;
  uint32_t maxWidth = 0, maxHeight = 0;
  std::wstring adapterName;
  std::string detail;
};

D3d12VeProbeResult ProbeD3D12VideoEncode(const LUID& adapterLuid);

}  // namespace wvd

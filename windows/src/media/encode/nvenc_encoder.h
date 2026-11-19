// Tier-1 NVENC module. Compiles to an "unavailable" stub unless the NVIDIA
// Video Codec SDK headers are on the include path
// (-DWVD_NVENC_SDK_DIR=<sdk>; §4.6). The runtime (nvEncodeAPI64.dll) is
// loaded dynamically so the binary runs on AMD/Intel machines too.
//
// On the session's D3D11 device the NV12 textures are registered and encoded
// in place (zero-copy). Without a shared device, packed CPU NV12 is uploaded
// to a texture on the selected GPU. Each synchronous encode maps/unmaps its
// input and owns its output buffer.
#pragma once

#include "iencoder.h"

namespace wvd {

bool NvencRuntimeAvailable();  // DLL + matching API version; session checked at creation
bool NvencModuleBuilt();       // false when compiled without the SDK

std::unique_ptr<IVideoEncoder> CreateNvencEncoder(const EncoderInitDesc& d);

}  // namespace wvd

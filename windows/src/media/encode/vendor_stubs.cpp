// Tier-1 AMF module (runtime probe only).
//
// Status per plan §4.6: the native AMF encode implementation lands in Phase 0b
// (RDNA2/3/4 profiles). Until then the factory reports AMF as detected-but-not-
// implemented, and AMD GPUs are driven through their hardware MFT (mf-hw in
// mf_encoder.cpp), which is a real low-latency HEVC/H.264 encoder.
#include "iencoder.h"

#include "wvd/logging.h"

namespace wvd {

bool AmfRuntimeAvailable() {
  static HMODULE mod = [] {
    // System32 only (§4.7-6): never let a stray amfrt64.dll next to the
    // executable or on PATH be loaded by the media worker.
    HMODULE m = LoadLibraryExA("amfrt64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) WVD_LOGV("amfrt64.dll not present (no AMD driver/runtime here)");
    return m;
  }();
  return mod != nullptr;
}

std::unique_ptr<IVideoEncoder> CreateAmfEncoder(const EncoderInitDesc&) {
  if (!AmfRuntimeAvailable()) return nullptr;
  WVD_LOGW("AMF module: runtime detected, native implementation pending (Phase 0b); "
           "the AMD hardware MFT (mf-hw) is used instead (§4.6)");
  return nullptr;
}

}  // namespace wvd

#pragma once

#include "capture/dda_capture.h"
#include "depth/game_depth_ipc.h"
#include "depth/synthetic_depth.h"

#include <memory>

namespace wvd {

// Capture-thread reader. Both colour and depth are copied from one record.
// A stale or mismatched record cannot contribute depth to desktop capture.
class GameDepthCapture {
 public:
  ~GameDepthCapture();
  bool Capture(const RECT& monitor, LUID adapter, uint32_t fps, FrameRing& ring,
               CapturedFrame& frame, std::shared_ptr<const DepthFrame>& depth);
  void StopRequest();
  bool HasRecentFrame(int64_t now) const;
  bool Attached(int64_t now) const;

 private:
  game_depth::Mapping mapping_;
  int64_t token_ = 0;
  int64_t lastFrame_ = 0;
  int64_t lastAttached_ = 0;
  bool depthSeen_ = false;
  std::vector<uint32_t> bgra_;
};

}  // namespace wvd

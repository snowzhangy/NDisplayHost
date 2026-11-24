#pragma once

#include <d3d11_4.h>

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
  bool CheckSize(uint32_t width, uint32_t height);
  bool HasRecentFrame(int64_t now) const;
  bool Attached(int64_t now) const;

 private:
  bool OpenTarget(HWND window, int64_t now);
  bool CopyPicture(FrameRing& ring, uint32_t slot, game_depth::SharedFrames& shared);

  game_depth::Mapping mapping_;
  game_depth::Handle pipe_, game_;
  HWND window_ = nullptr;
  DWORD gamePid_ = 0;
  bool offered_ = false;
  int64_t retryAt_ = 0;
  int64_t offeredAt_ = 0;
  uint32_t unsupportedW_ = 0, unsupportedH_ = 0;
  int64_t token_ = 0;
  int64_t lastFrame_ = 0;
  int64_t lastAttached_ = 0;
  bool depthSeen_ = false;
  uint64_t producerId_ = 0;
  bool gpuErrorLogged_ = false;
  ComPtr<ID3D11Fence> releaseFence_;
  struct Source {
    ComPtr<ID3D11Texture2D> texture;
    uint64_t id = 0;
  };
  std::array<Source, game_depth::kSlotCount> sources_;
};

}  // namespace wvd

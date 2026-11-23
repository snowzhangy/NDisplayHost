#include "depth/game_depth_capture.h"

#include "wvd/time.h"

namespace wvd {

namespace {

HWND FullscreenForeground(const RECT& monitor) {
  const HWND window = GetForegroundWindow();
  RECT bounds{};
  POINT origin{};
  if (!window || IsIconic(window) || !GetClientRect(window, &bounds) ||
      !ClientToScreen(window, &origin)) return nullptr;
  OffsetRect(&bounds, origin.x, origin.y);
  return EqualRect(&bounds, &monitor) ? window : nullptr;
}

}  // namespace

GameDepthCapture::~GameDepthCapture() { StopRequest(); }

void GameDepthCapture::StopRequest() {
  if (!token_) return;
  game_depth::Mapping::Access access(mapping_);
  if (access.frames && access.frames->request.token == token_) access.frames->request = {};
  token_ = 0;
  lastFrame_ = 0;
  depthSeen_ = false;
}

bool GameDepthCapture::HasRecentFrame(int64_t now) const {
  return token_ && depthSeen_ && lastFrame_ && now - lastFrame_ < MsToQpc(100);
}

bool GameDepthCapture::Attached(int64_t now) const {
  return lastAttached_ && now - lastAttached_ < MsToQpc(2000);
}

bool GameDepthCapture::Capture(const RECT& monitor, LUID adapter, uint32_t fps, FrameRing& ring,
                               CapturedFrame& frame, std::shared_ptr<const DepthFrame>& depth) {
  const uint32_t width = ring.width(), height = ring.height();
  const HWND window = FullscreenForeground(monitor);
  if (!window || !game_depth::SupportedSize(width, height)) {
    StopRequest();
    return false;
  }
  if (!mapping_.Open(true)) return false;
  const int64_t now = QpcNow();
  game_depth::Frame incoming;
  {
    game_depth::Mapping::Access access(mapping_);
    if (!access.frames) return false;
    auto& shared = *access.frames;
    auto& request = shared.request;
    const auto windowId = reinterpret_cast<uint64_t>(window);
    if (request.token != token_ && request.owner != GetCurrentProcessId() && request.expires > now)
      return false;
    if (!token_ || request.token != token_ || request.window != windowId ||
        request.width != width || request.height != height || request.expires <= now) {
      token_ = now;
      lastFrame_ = 0;
      depthSeen_ = false;
    }
    request = {windowId, token_, now + MsToQpc(500), adapter, width, height, fps,
               GetCurrentProcessId()};

    incoming = shared.frame;
    if (incoming.token != token_ || incoming.qpc <= lastFrame_ || incoming.qpc > now ||
        now - incoming.qpc >= MsToQpc(100) || incoming.width != width || incoming.height != height)
      return false;
    lastFrame_ = incoming.qpc;
    if (!incoming.depthValid && !depthSeen_) return false;
    const size_t pixels = size_t(width) * height;
    bgra_.assign(shared.bgra, shared.bgra + pixels);
    if (incoming.depthValid) {
      auto map = std::make_shared<DepthFrame>();
      map->valid = true;
      map->mapWidth = width / 4;
      map->mapHeight = height / 16 * 4;
      const size_t count = size_t(map->mapWidth) * map->mapHeight;
      map->v.assign(shared.depth, shared.depth + count);
      depth = std::move(map);
    }
  }
  depthSeen_ = true;
  lastAttached_ = incoming.qpc;
  frame.qpcCapture = incoming.qpc;
  frame.ringSlot = ring.AcquireWriteSlot();
  ring.WriteCpuFrame(frame.ringSlot, reinterpret_cast<const uint8_t*>(bgra_.data()), width * 4);
  frame.qpcCopyDone = QpcNow();
  frame.copied = true;
  frame.hadDirtyRects = true;
  return true;
}

}  // namespace wvd

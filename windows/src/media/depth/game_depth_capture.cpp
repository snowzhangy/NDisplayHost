#include "depth/game_depth_capture.h"

#include <algorithm>

#include "wvd/logging.h"
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
  {
    game_depth::Mapping::Access access(mapping_);
    if (access.frames) access.frames->request = {};
  }
  if (pipe_) DisconnectNamedPipe(pipe_.Get());
  pipe_.Reset();
  game_.Reset();
  mapping_.Close();
  window_ = nullptr;
  gamePid_ = 0;
  offered_ = false;
  token_ = 0;
  lastFrame_ = 0;
  depthSeen_ = false;
  producerId_ = 0;
  releaseFence_.Reset();
  sources_ = {};
}

bool GameDepthCapture::CheckSize(uint32_t width, uint32_t height) {
  if (game_depth::SupportedSize(width, height)) {
    unsupportedW_ = unsupportedH_ = 0;
    return true;
  }
  if (width != unsupportedW_ || height != unsupportedH_) {
    WVD_LOGW("game depth unavailable at %ux%u: requires even dimensions, at least 256x128 and at most 8294400 pixels; using desktop capture",
             width, height);
    unsupportedW_ = width;
    unsupportedH_ = height;
  }
  return false;
}

bool GameDepthCapture::HasRecentFrame(int64_t now) const {
  return token_ && depthSeen_ && lastAttached_ && now - lastAttached_ < MsToQpc(100);
}

bool GameDepthCapture::Attached(int64_t now) const {
  return lastAttached_ && now - lastAttached_ < MsToQpc(2000);
}

bool GameDepthCapture::OpenTarget(HWND window, int64_t now) {
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  if (window != window_ || gamePid_ != pid) {
    StopRequest();
    window_ = window;
    gamePid_ = pid;
    retryAt_ = 0;
  }
  if (mapping_) {
    if (WaitForSingleObject(game_.Get(), 0) == WAIT_TIMEOUT) return true;
    StopRequest();
    return false;
  }
  if (now < retryAt_) return false;
  retryAt_ = now + MsToQpc(250);
  if (!pid || !game_depth::SameSession(pid)) return false;
  game_.Reset(OpenProcess(PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, pid));
  if (!game_) return false;
  pipe_.Reset(game_depth::CreateOfferPipe(window));
  if (!pipe_) {
    WVD_LOGV("game depth rendezvous unavailable for PID %lu (Win32 %lu)", pid, GetLastError());
    return false;
  }
  if (mapping_.Create()) return true;
  pipe_.Reset();
  return false;
}

bool GameDepthCapture::CopyPicture(FrameRing& ring, uint32_t slot, game_depth::SharedFrames& shared) {
  const auto& frame = shared.frame;
  if (producerId_ != shared.producerId) {
    producerId_ = shared.producerId;
    releaseFence_.Reset();
    sources_ = {};
  }
  if (!releaseFence_) {
    game_depth::Handle handle;
    ComPtr<ID3D11Device5> device;
    if (!game_depth::DuplicateFrom(game_.Get(), shared.releaseFence, handle) ||
        FAILED(ring.Device()->QueryInterface(IID_PPV_ARGS(&device))) ||
        FAILED(device->OpenSharedFence(handle.Get(), IID_PPV_ARGS(&releaseFence_)))) return false;
  }
  auto& source = sources_[frame.slot];
  if (!source.texture || source.id != frame.textureId) {
    game_depth::Handle handle;
    ComPtr<ID3D11Device1> device;
    ComPtr<ID3D11Texture2D> texture;
    if (!game_depth::DuplicateFrom(game_.Get(), frame.texture, handle) ||
        FAILED(ring.Device()->QueryInterface(IID_PPV_ARGS(&device))) ||
        FAILED(device->OpenSharedResource1(handle.Get(), IID_PPV_ARGS(&texture)))) return false;
    source.texture = std::move(texture);
    source.id = frame.textureId;
  }
  ComPtr<ID3D11DeviceContext> context;
  ring.Device()->GetImmediateContext(&context);
  ComPtr<ID3D11DeviceContext4> context4;
  if (FAILED(context.As(&context4)) || FAILED(ring.WriteGpuFrame(slot, source.texture.Get())))
    return false;
  // Publish the release value under the same lock that excludes the producer
  // choosing this slot. A failed Signal retires the texture on the game side.
  const bool signalled = SUCCEEDED(context4->Signal(releaseFence_.Get(), frame.sequence));
  shared.releases[frame.slot] = signalled ? frame.sequence : UINT64_MAX;
  context->Flush();
  return signalled;
}

bool GameDepthCapture::Capture(const RECT& monitor, LUID adapter, uint32_t fps, FrameRing& ring,
                               CapturedFrame& frame, std::shared_ptr<const DepthFrame>& depth) {
  const uint32_t width = ring.width(), height = ring.height();
  const HWND window = FullscreenForeground(monitor);
  if (!window) {
    StopRequest();
    return false;
  }
  const int64_t now = QpcNow();
  if (!OpenTarget(window, now)) return false;
  game_depth::Mapping::Access access(mapping_);
  if (!access.frames) return false;
  auto& shared = *access.frames;
  auto& request = shared.request;
  if (!token_ || request.width != width || request.height != height || request.expires <= now) {
    token_ = now;
    lastFrame_ = 0;
    depthSeen_ = false;
  }
  request = {reinterpret_cast<uint64_t>(window), token_, now + MsToQpc(500), adapter, width, height, fps};

  // Re-arm the rendezvous if the add-on runtime was recreated. The mapping stays
  // alive so outstanding GPU releases are still visible to its old producer.
  const int64_t lastOfferProgress = (std::max)(offeredAt_, lastFrame_);
  if (offered_ && now - lastOfferProgress >= MsToQpc(2000)) {
    DisconnectNamedPipe(pipe_.Get());
    offered_ = false;
  }
  if (!offered_ && game_depth::OfferMapping(pipe_.Get(), gamePid_, mapping_)) {
    offered_ = true;
    offeredAt_ = now;
  }

  const auto& incoming = shared.frame;
  if (!shared.producerId || incoming.token != token_ || incoming.qpc <= lastFrame_ ||
      incoming.qpc > now || now - incoming.qpc >= MsToQpc(100) ||
      incoming.width != width || incoming.height != height ||
      incoming.slot >= game_depth::kSlotCount || !incoming.sequence ||
      incoming.sequence <= shared.releases[incoming.slot]) return false;
  lastFrame_ = incoming.qpc;
  if (!incoming.depthValid && !depthSeen_) return false;

  frame.ringSlot = ring.AcquireWriteSlot();
  if (!CopyPicture(ring, frame.ringSlot, shared)) {
    if (!gpuErrorLogged_)
      WVD_LOGW("game depth: could not import or copy the game's shared GPU texture; using desktop capture");
    gpuErrorLogged_ = true;
    return false;
  }
  gpuErrorLogged_ = false;
  if (incoming.depthValid) {
    auto map = std::make_shared<DepthFrame>();
    map->valid = true;
    map->mapWidth = width / 4;
    map->mapHeight = height / 16 * 4;
    const size_t count = size_t(map->mapWidth) * map->mapHeight;
    map->v.assign(shared.depth, shared.depth + count);
    depth = std::move(map);
  }
  depthSeen_ = true;
  lastAttached_ = incoming.qpc;
  frame.qpcCapture = incoming.qpc;
  frame.qpcCopyDone = QpcNow();
  frame.copied = true;
  frame.hadDirtyRects = true;
  return true;
}

}  // namespace wvd

#include "session/media_session.h"
#include "session/link_rate.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "encode/mf_encoder.h"
#include "wvd/logging.h"
#include "wvd/pacing.h"
#include "wvd/time.h"

namespace wvd {

namespace {
// Encoding waits while this many encoded pictures are still queued for the
// wire (SessionConfig::backpressureMs bounds the wait).
constexpr size_t kSendBackpressureDepth = 2;
constexpr uint64_t kDesktopEncodePixels = 3840ull * 2160;

void FitToBounds(uint32_t& width, uint32_t& height, uint32_t maxWidth, uint32_t maxHeight) {
  if (maxWidth && width > maxWidth &&
      (!maxHeight || uint64_t(maxWidth) * height <= uint64_t(maxHeight) * width)) {
    height = uint32_t(uint64_t(height) * maxWidth / width);
    width = maxWidth;
  } else if (maxHeight && height > maxHeight) {
    width = uint32_t(uint64_t(width) * maxHeight / height);
    height = maxHeight;
  }
}

void FitDecoderBudget(uint32_t& width, uint32_t& height, uint32_t fps,
                      uint64_t maxPixels, VideoDecodeLimits limits) {
  if (limits.pixelsPerSecond) {
    const auto peerPixels = (std::max)(uint64_t(1), limits.pixelsPerSecond / fps);
    maxPixels = maxPixels ? (std::min)(maxPixels, peerPixels) : peerPixels;
  }
  FitToBounds(width, height, limits.width, limits.height);
  FitEncodeSize(width, height, maxPixels, width, height);
}

bool FullStereoNeedsHevc(const StereoEncodePlan& plan) {
  return (plan.layout == msg::StereoLayout::SideBySide ||
          plan.layout == msg::StereoLayout::TopBottom) &&
         (plan.width > 4096 || plan.height > 2304 ||
          uint64_t(plan.width) * plan.height > kDesktopEncodePixels);
}

uint32_t StereoHevcLevel(const StereoEncodePlan& plan) {
  if (plan.layout == msg::StereoLayout::None) return 0;
  constexpr uint64_t kLevel5PictureSamples = 8912896;
  constexpr uint64_t kLevel5SampleRate = 267386880;
  const uint64_t samples = uint64_t(plan.width) * plan.height;
  const uint64_t sampleRate = samples * plan.fps;
  // ITU-T H.265 Annex A. Supply levels for the wide and high-rate cases
  // where a hardware MFT can default to a level below the stream's rate.
  if (samples <= kLevel5PictureSamples) {
    if (sampleRate <= kLevel5SampleRate) return 0;
    if (sampleRate <= 2 * kLevel5SampleRate) return 153;  // 5.1
    if (sampleRate <= 4 * kLevel5SampleRate) return 156;  // 5.2
  }
  if (sampleRate <= 4 * kLevel5SampleRate) return 180;    // 6
  if (sampleRate <= 8 * kLevel5SampleRate) return 183;    // 6.1
  return 186;                                            // 6.2
}

// RAII over a CRITICAL_SECTION (the re-entrant side of the encoder lock).
struct CsGuard {
  explicit CsGuard(CRITICAL_SECTION& cs) : cs_(cs) { EnterCriticalSection(&cs_); }
  ~CsGuard() { LeaveCriticalSection(&cs_); }
  CRITICAL_SECTION& cs_;
};

void NormalizeDepthMap(const DepthFrame& map, uint32_t width, uint32_t height,
                       const uint8_t* lut, std::vector<uint8_t>& output) {
  output.resize(size_t(width) * height);
  if (width == map.mapWidth && height == map.mapHeight) {
    for (size_t i = 0; i < output.size(); ++i) output[i] = lut[map.v[i]];
    return;
  }
  // The game's back buffer can be larger than the colour encode size.
  // Preserve foreground geometry when reducing its depth map to fit.
  for (uint32_t y = 0; y < height; ++y) {
    const uint32_t y0 = y * map.mapHeight / height;
    const uint32_t y1 = (std::max)(y0 + 1, (y + 1) * map.mapHeight / height);
    for (uint32_t x = 0; x < width; ++x) {
      const uint32_t x0 = x * map.mapWidth / width;
      const uint32_t x1 = (std::max)(x0 + 1, (x + 1) * map.mapWidth / width);
      uint16_t nearest = 0;
      for (uint32_t row = y0; row < y1; ++row)
        for (uint32_t col = x0; col < x1; ++col)
          nearest = (std::max)(nearest, map.v[size_t(row) * map.mapWidth + col]);
      output[size_t(y) * width + x] = lut[nearest];
    }
  }
}
}  // namespace

void FitEncodeSize(uint32_t srcW, uint32_t srcH, uint64_t maxPixels, uint32_t& outW,
                   uint32_t& outH) {
  outW = srcW & ~1u;
  outH = srcH & ~1u;
  if (!maxPixels || uint64_t(outW) * outH <= maxPixels) return;
  const double scale = std::sqrt(double(maxPixels) / (double(srcW) * double(srcH)));
  outW = (std::max)(2u, uint32_t(double(srcW) * scale) & ~1u);
  outH = (std::max)(2u, uint32_t(double(srcH) * scale) & ~1u);
}

StereoEncodePlan PlanStereoEncode(uint32_t width, uint32_t height, uint32_t fps,
                                  msg::StereoLayout layout, uint64_t maxPixels,
                                  VideoDecodeLimits limits, bool forceHalfSbs,
                                  StereoOutputSize output) {
  using msg::StereoLayout;
  if (!fps) fps = 60;
  if (layout == StereoLayout::SideBySide) {
    uint32_t fullWidth = width, fullHeight = height;
    FitToBounds(fullWidth, fullHeight, output.width, output.height);
    const uint64_t pixels = uint64_t(fullWidth) * fullHeight;
    const uint32_t fullRate = pixels > kDesktopEncodePixels ? (std::min)(fps, 60u) : fps;
    if (!forceHalfSbs && !output.halfSbs && (!maxPixels || pixels <= maxPixels) &&
        (!limits.width || fullWidth <= limits.width) &&
        (!limits.height || fullHeight <= limits.height) &&
        (!limits.pixelsPerSecond || pixels <= limits.pixelsPerSecond / fullRate)) {
      width = fullWidth;
      height = fullHeight;
      fps = fullRate;
    } else {
      width /= 2;   // full height, half the horizontal samples in each eye
      layout = StereoLayout::SideBySideHalf;
      FitToBounds(width, height, output.halfSbs ? output.width : output.width / 2,
                  output.height);
    }
  } else {
    FitToBounds(width, height, output.width, output.height);
  }
  FitDecoderBudget(width, height, fps, maxPixels, limits);
  // A 4:2:0 chroma sample spans two pixels. Each eye must start on an
  // even coordinate so chroma never straddles the seam during conversion.
  if (layout == StereoLayout::SideBySide || layout == StereoLayout::SideBySideHalf)
    width &= ~3u;
  else
    height &= ~3u;
  return {width, height, fps, layout};
}

StereoEncodePlan MediaSession::ChooseEncodePlan(UINT width, UINT height, bool forceHalfSbs) const {
  const auto layout = cfg_.stereoLayout;
  const uint32_t fps = cfg_.fps ? cfg_.fps : 60;
  const uint64_t maxPixels = cfg_.maxEncodePixels.value_or(
      layout == msg::StereoLayout::SideBySide ? 2 * kDesktopEncodePixels : kDesktopEncodePixels);
  const VideoDecodeLimits limits = channel_ ? channel_->GetVideoDecodeLimits() : VideoDecodeLimits{};
  if (layout == msg::StereoLayout::None) {
    FitDecoderBudget(width, height, fps, maxPixels, limits);
    return {width, height, fps, layout};
  }
  const bool h264 = cfg_.codec == "h264" || cfg_.codec == "mf-sw-h264";
  auto plan = PlanStereoEncode(width, height, fps, layout, maxPixels, limits, forceHalfSbs,
                               cfg_.stereoOutputSize);
  if (h264 && layout == msg::StereoLayout::SideBySide && FullStereoNeedsHevc(plan))
    plan = PlanStereoEncode(width, height, fps, layout, maxPixels, limits, true,
                            cfg_.stereoOutputSize);
  return plan;
}

void MediaSession::ConfigureEncodeSizeLocked(UINT width, UINT height, bool forceHalfSbs) {
  const auto plan = ChooseEncodePlan(width, height, forceHalfSbs);
  encSourceW_ = width;
  encSourceH_ = height;
  encDesc_.width = encColorW_ = plan.width;
  encDesc_.height = encColorH_ = plan.height;
  encDesc_.fpsNum = plan.fps;
  encDesc_.hevcOnly = FullStereoNeedsHevc(plan);
  encDesc_.hevcLevel = StereoHevcLevel(plan);
  encodedStereoLayout_ = plan.layout;
  encodeRateLimit_ = plan.layout != msg::StereoLayout::None ? plan.fps : 0;
  if (cfg_.stereoLayout == msg::StereoLayout::SideBySide &&
      plan.layout == msg::StereoLayout::SideBySideHalf)
    WVD_LOGI("stereo: half side-by-side from %ux%u, encoding %ux%u@%u", width, height,
             plan.width, plan.height, plan.fps);
}

bool MediaSession::FallbackToHalfSbsLocked(UINT width, UINT height) {
  if (encodedStereoLayout_ != msg::StereoLayout::SideBySide) return false;
  WVD_LOGW("encoder could not stream full side-by-side at %ux%u@%u; trying half side-by-side",
           encDesc_.width, encDesc_.height, encDesc_.fpsNum);
  ConfigureEncodeSizeLocked(width, height, true);
  if (!RecreateEncoderLocked()) return false;
  forceFullFrame_ = true;
  return true;
}

MediaSession::MediaSession(SessionConfig cfg, GenCounters& gens)
    : cfg_(std::move(cfg)), gens_(gens), cursor_(std::make_unique<CursorSource>(gens)) {
  sendWake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  InitializeCriticalSection(&encCs_);
}

MediaSession::~MediaSession() {
  Stop();
  if (sendWake_) CloseHandle(sendWake_);
  DeleteCriticalSection(&encCs_);
}

// ---------------------------------------------------------------------------
// Start / Stop
// ---------------------------------------------------------------------------

HRESULT MediaSession::Start(std::shared_ptr<MediaChannel> channel) {
  if (running_) return E_UNEXPECTED;
  // Coordinates below come from DXGI/the desktop in PHYSICAL pixels; make sure
  // this process is per-monitor DPI aware before reading any of them.
  EnablePerMonitorDpiAwareness();
  channel_ = std::move(channel);

  factory_ = CreateDxgiFactory();
  if (!factory_) return E_FAIL;

  HRESULT hr;
  UINT w = 0, h = 0;
  if (synthetic) {
    // No display needed (headless / CI). Default hardware device, fixed size.
    w = cfg_.synWidth;
    h = cfg_.synHeight;
    D3D_FEATURE_LEVEL fl{};
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                           &device_, &fl, &ctx_);
    if (FAILED(hr)) {
      WVD_LOGE("synthetic: D3D11 device creation failed hr=%s", HresultStr(hr).c_str());
      return hr;
    }
    EnableD3D11ThreadSafety(device_.Get());
    RaiseGpuPriority(device_.Get());
    // The encoder lives on the same GPU as the source surfaces.
    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> ad;
    if (SUCCEEDED(device_.As(&dxgiDev)) && SUCCEEDED(dxgiDev->GetAdapter(&ad))) {
      DXGI_ADAPTER_DESC d{};
      if (SUCCEEDED(ad->GetDesc(&d))) outputInfo_.adapterLuid = d.AdapterLuid;
    }
    WVD_LOGI("SYNTHETIC source %ux%u (no display; §4.1 full-frame rule still holds)",
             w, h);
  } else {
    DxOutputInfo oi;
    if (!FindOutput(factory_.Get(),
                    cfg_.monitorName.empty() ? nullptr : cfg_.monitorName.c_str(), oi)) {
      WVD_LOGE("no matching output for '%ls'", cfg_.monitorName.c_str());
      for (const auto& active : EnumerateOutputs(factory_.Get()))
        WVD_LOGI("available output: %ls", active.monitorName.c_str());
      return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    outputInfo_ = oi;
    WVD_LOGI("capturing %ls (%ldx%ld, adapter luid %lu:%lu)", oi.monitorName.c_str(),
             LONG(oi.desktopCoords.right - oi.desktopCoords.left),
             LONG(oi.desktopCoords.bottom - oi.desktopCoords.top),
             (unsigned long)oi.adapterLuid.HighPart, oi.adapterLuid.LowPart);

    hr = CreateDeviceForAdapter(factory_.Get(), oi.adapterLuid, device_, ctx_);
    if (FAILED(hr)) return hr;

    // Open the output object on this adapter for DuplicateOutput.
    output_ = FindDxgiOutput(factory_.Get(), oi.adapterLuid, oi.monitorName);
    if (!output_) return E_FAIL;

    // Start duplication FIRST and take the ring size from it (§4.1). Desktop
    // coordinates are DPI-scaled when awareness is wrong; the duplication is
    // always the real pixel size, so it — not the coords — defines the surface.
    dda_ = std::make_unique<DdaCapture>();
    hr = dda_->Start(DdaCapture::InitDesc{output_, device_, ctx_, nullptr});
    if (FAILED(hr)) return hr;
    w = dda_->width();
    h = dda_->height();
    if (!w || !h) {
      WVD_LOGE("duplication reported a zero-sized output");
      return E_FAIL;
    }
    const RECT& dc = outputInfo_.desktopCoords;
    if (w != UINT(dc.right - dc.left) || h != UINT(dc.bottom - dc.top)) {
      WVD_LOGW("capture %ux%u differs from desktop coords %ldx%ld — using the capture "
               "(check DPI awareness)",
               w, h, LONG(dc.right - dc.left), LONG(dc.bottom - dc.top));
    }
  }

  hr = ring_.Init(device_.Get(), w, h, 3);
  if (FAILED(hr)) return hr;
  hr = converter_.Init(device_.Get(), ctx_.Get());
  if (FAILED(hr)) return hr;
  converterReady_ = true;

  if (dda_) {
    dda_->SetRing(&ring_);
    cursor_->NoteSourceRecreated();
  }
  forceFullFrame_ = true;   // first picture is owed even on a static desktop

  // Encoder (§4.6 tiers). It shares the capture device, so hardware tiers
  // take the converted texture directly.
  ConfigureEncodeSizeLocked(w, h);
  if (encDesc_.width != w || encDesc_.height != h)
    WVD_LOGI("encode size %ux%u for a %ux%u source", encDesc_.width, encDesc_.height, w, h);
  if (cfg_.stereoLayout == msg::StereoLayout::None && cfg_.depthTestPattern) {
    depthSrc_ = std::make_unique<SyntheticDepthSource>(cfg_.depthTestDropEvery);
    WVD_LOGI("depth: synthetic test source attached (--depth-test-pattern%s); packing waits "
             "for the headset's request",
             cfg_.depthTestDropEvery ? ", with DEPTH_VALID gaps" : "");
    depthEval_.store(true);
  } else if (cfg_.stereoLayout == msg::StereoLayout::None && cfg_.gameDepth && dda_) {
    gameDepth_ = std::make_unique<GameDepthCapture>();
    WVD_LOGI("game depth enabled: waiting for a headset request and a full-screen game with the NDisplay add-on");
    gameDepth_->CheckSize(w, h);
  }
  encDesc_.fpsDen = 1;
  encDesc_.preferQuality = !channel_ || channel_->PreferVideoQuality();
  encDesc_.bitrateKbps = cfg_.bitrateKbps;
  encDesc_.adapterLuid = outputInfo_.adapterLuid;
  encDesc_.device = device_.Get();
  encDesc_.gopSeconds = cfg_.gopSeconds;
  encDesc_.vui = kSdrReferenceVui;
  encDesc_.codec = cfg_.codec == "h264" ? Codec::H264 : Codec::HEVC;
  if (cfg_.codec == "null")
    encDesc_.pinEncoder = "null-test";
  else if (cfg_.codec != "auto" && cfg_.codec != "hevc" && cfg_.codec != "h264")
    encDesc_.pinEncoder = cfg_.codec;
  {
    std::lock_guard<std::mutex> lk(encMutex_);
    encoder_ = CreateBestEncoder(encDesc_, &encoderName_);
    if (!encoder_) FallbackToHalfSbsLocked(w, h);
    if (encoder_) PublishEncodeModeLocked();
    codecConfigPending_ = true;
    csd_.clear();
  }
  if (!encoder_) {
    WVD_LOGE("no encoder available (run ndisplay diag caps; §4.6 tiers)");
    return E_FAIL;
  }

  // Layout: current outputs → MonitorDesc list.
  captureDevice_ = synthetic ? std::wstring() : outputInfo_.monitorName;
  {
    std::lock_guard<std::mutex> lk(layoutMutex_);
    BuildLayoutLocked(w, h);
  }

  if (!cfg_.inputPipe.empty()) {
    if (cfg_.inputToken == 0) {
      // No token: the helper would refuse both InputArm and InputInject. Input
      // stays off rather than silently half-working (§4.7-4).
      WVD_LOGW("input forwarding off: no --input-token (start 'ndisplay input', pass the "
               "token it prints; §4.7)");
    } else if (!EnsureInputArmed()) {
      WVD_LOGW("input helper not reachable/armed yet — retried while the session runs");
    }
  }

  QueryPerformanceCounter(&lastStatsTick_);
  running_ = true;
  stopReq_ = false;

  // Someone is watching this desktop through the headset: the display must
  // not idle off underneath them (a sleeping display stops desktop
  // composition, so capture would deliver nothing).
  if (channel_ && !synthetic) keepAwake_.Start(L"WVD: streaming this display to a headset");
  // Like the Mac's display while Mac Virtual Display runs: this PC's own
  // screens go black, the stream does not (the curtain is hidden from capture).
  if (channel_ && !synthetic && cfg_.screenOff) {
    ScreenCurtain::Options co;
    co.capturedDevice = captureDevice_;
    curtain_.Start(co);
    const auto cs = curtain_.status();
    WVD_LOGI("screen off: %d PC screen(s) blacked out%s; %s", cs.covered,
             cs.skipped ? ", the streamed one left on (this Windows version cannot hide a "
                          "window from capture)"
                        : "",
             cs.hotkey ? "Ctrl+Alt+Shift+B shows them" : "Ctrl+Alt+Shift+B is taken by another app");
  }

  if (channel_) {
    SendMonitorLayout();
    SendDisplayModes(0, ModeChangeStatus::None);
  }
  // CodecConfig is queued by the encode thread right before the first picture
  // (it needs the encoder's parameter sets).

  WVD_LOGI("media session started: source %ux%u, encode %ux%u@%u enc=%s input=%s "
           "sessionGen=%llu",
           w, h, encDesc_.width, encDesc_.height, encDesc_.fpsNum, encoderName_.c_str(),
           encoder_->zeroCopy() ? "gpu-texture" : "cpu-copy",
           (unsigned long long)gens_.sessionGen.load());
  thCapture_ = std::thread([this] { synthetic ? CaptureSynthetic() : CaptureDda(); });
  thEncode_ = std::thread([this] { EncodeLoop(); });
  thSend_ = std::thread([this] { SendLoop(); });
  if (channel_) thRecv_ = std::thread([this] { RecvLoop(); });
  return S_OK;
}

void MediaSession::Stop() {
  stopReq_ = true;
  curtain_.Stop();  // the PC's screens come back as soon as the headset is gone
  if (sendWake_) SetEvent(sendWake_);
  if (channel_) channel_->Close();  // unblocks a send stuck on a dead peer
  if (thCapture_.joinable()) thCapture_.join();
  if (thEncode_.joinable()) thEncode_.join();
  if (thSend_.joinable()) thSend_.join();
  if (thRecv_.joinable()) thRecv_.join();
  // SetAudioWanted owns audio_ on the recv thread. Read it only after joining that
  // thread, so a hello concurrent with shutdown cannot race a unique_ptr access.
  // stopReq_ already prevents the audio sink from enqueuing during shutdown.
  if (audio_) audio_->Stop();
  // Nothing may stay pressed once the headset is gone (§4.4).
  ReleaseAndDisarmInput();
  running_ = false;
  keepAwake_.Stop();
  if (dda_) dda_->Stop();
  // A size picked on the headset lasts for the session only.
  RestoreModeIfChanged();
  ring_.Deinit();
  channel_.reset();
  std::lock_guard<std::mutex> lk(encMutex_);
  encoder_.reset();
}

// ---------------------------------------------------------------------------
// Control packets
// ---------------------------------------------------------------------------

void MediaSession::PushUrgent(std::vector<uint8_t> pkt) {
  PacketHeader h{};
  if (pkt.size() >= sizeof(h)) std::memcpy(&h, pkt.data(), sizeof(h));
  if (PacketKind(h.kind) == PacketKind::CursorPos) {
    cursorQueue_.Push(std::move(pkt));
  } else if (!controlQueue_.Push(std::move(pkt))) {
    WVD_LOGW("control queue full — peer is not draining (packet kind %u dropped)", h.kind);
  }
  SetEvent(sendWake_);
}

void MediaSession::PushMedia(SendWork sw) {
  if (!channel_) return;  // local demo: nothing drains the queue
  if (!sendQueue_.Push(std::move(sw))) {
    {
      std::lock_guard<std::mutex> lk(statsMutex_);
      ++lateDrops_;
    }
    // A dropped encoded P-frame breaks references for all later pictures.
    // Retain the earlier queue and force the NEXT encode to start a new chain.
    std::lock_guard<std::mutex> lk(encMutex_);
    if (encoder_) encoder_->ForceKeyframe();
    forceFullFrame_ = true;
  }
  SetEvent(sendWake_);
}

void MediaSession::BuildLayoutLocked(UINT w, UINT h) {
  MonitorDesc m{};
  const auto id = synthetic ? std::string("SYNTH0") : WideToUtf8(outputInfo_.monitorName.c_str());
  memcpy(m.id, id.data(), (std::min)(id.size(), sizeof(m.id) - 1));
  m.width = w;
  m.height = h;
  m.dpiX = m.dpiY = 96;
  m.refreshMilliHz = (cfg_.fps ? cfg_.fps : 60) * 1000;
  m.primary = 1;
  if (!synthetic) {
    const auto& rect = outputInfo_.desktopCoords;
    m.x = rect.left;
    m.y = rect.top;
    const POINT dpi = MonitorDpi(rect);
    m.dpiX = UINT(dpi.x);
    m.dpiY = UINT(dpi.y);
    DisplayModeInfo current;
    if (CurrentDisplayMode(outputInfo_.monitorName, current)) m.refreshMilliHz = current.refreshHz * 1000;
    if (dda_ && dda_->refreshMilliHz()) m.refreshMilliHz = dda_->refreshMilliHz();
  }
  // The app opens a window and a receiver for every advertised monitor.
  // Keep the selected output's desktop offset for pointer mapping.
  monitors_ = {m};
  layout_.Update(monitors_);
  WVD_LOGI("layout: sharing %s %ux%u at (%d,%d)", m.id, w, h, m.x, m.y);
}

void MediaSession::SendMonitorLayout() {
  std::lock_guard<std::mutex> lk(layoutMutex_);
  SendMonitorLayoutLocked();
}

void MediaSession::SendMonitorLayoutLocked() {
  PacketWriter w(PacketKind::MonitorLayout);
  MonitorLayoutPayload ml{};
  ml.layoutGen = gens_.layoutGen.load();
  ml.count = uint32_t(monitors_.size());
  ml.captureIndex = kCaptureMonitorIndex;
  w.Append(ml);
  w.AppendArray(monitors_);
  PushUrgent(w.Take());
}

// ---------------------------------------------------------------------------
// Display size (the headset's size menu)
// ---------------------------------------------------------------------------

void MediaSession::SendDisplayModes(uint32_t requestId, ModeChangeStatus status) {
  std::lock_guard<std::mutex> lk(layoutMutex_);
  SendDisplayModesLocked(requestId, status);
}

void MediaSession::SendDisplayModesLocked(uint32_t requestId, ModeChangeStatus status) {
  std::vector<DisplayModeDesc> list;
  const uint16_t index = kCaptureMonitorIndex;
  EncodeMode activeMode;
  {
    std::lock_guard<std::mutex> lk(encodeModeMutex_);
    activeMode = encodeMode_;
  }
  auto describe = [&](uint32_t mw, uint32_t mh, uint32_t hz, bool current, bool native) {
    uint32_t flags = (current ? kModeCurrent : 0u) | (native ? kModeNative : 0u);
    auto plan = ChooseEncodePlan(mw, mh);
    if (current && mw == activeMode.sourceW && mh == activeMode.sourceH) {
      plan.width = activeMode.colorW;
      plan.height = activeMode.colorH;
    }
    if (plan.width != mw || plan.height != mh) flags |= kModeScaledForEncode;
    list.push_back(DisplayModeDesc{mw, mh, hz * 1000u, flags});
  };
  std::vector<DisplayModeInfo> modes;
  if (!captureDevice_.empty()) modes = ListDisplayModes(captureDevice_);
  if (!cfg_.allowModeChange || modes.empty()) {
    // Fixed size (synthetic source, mode change switched off, unknown device):
    // the current mode alone, so the headset shows no menu.
    const MonitorDesc& m = monitors_[index];
    DisplayModeInfo cur;
    const bool have = !captureDevice_.empty() && CurrentDisplayMode(captureDevice_, cur);
    describe(m.width, m.height, have ? cur.refreshHz : (cfg_.fps ? cfg_.fps : 60), true, true);
  } else {
    for (const auto& m : modes) describe(m.width, m.height, m.refreshHz, m.current, m.native);
  }

  DisplayModesPayload p{};
  p.layoutGen = gens_.layoutGen.load();
  p.monitor = index;
  p.count = uint16_t(list.size());
  p.requestId = requestId;
  p.status = uint32_t(status);
  PacketWriter w(PacketKind::DisplayModes);
  w.Append(p);
  w.AppendArray(list);
  PushUrgent(w.Take());
  if (status == ModeChangeStatus::None) {
    const auto cur = std::find_if(list.begin(), list.end(),
                                  [](const DisplayModeDesc& d) { return d.flags & kModeCurrent; });
    WVD_LOGI("display modes: %zu size(s) offered, current %ux%u@%u", list.size(),
             cur != list.end() ? cur->width : 0u, cur != list.end() ? cur->height : 0u,
             cur != list.end() ? cur->refreshMilliHz / 1000 : 0u);
  }
}

void MediaSession::HandleModeChange(PacketView pv) {
  auto r = pv.Get<ModeChangeRequest>();
  if (!r) return;
  const uint32_t hz = (r->refreshMilliHz + 500) / 1000;
  ModeChangeStatus status = ModeChangeStatus::Unsupported;
  if (r->monitor != kCaptureMonitorIndex) {
    WVD_LOGW("mode change for monitor %u, but this session streams monitor %u — refused",
             r->monitor, kCaptureMonitorIndex);
  } else if (captureDevice_.empty() || !cfg_.allowModeChange) {
    // Fixed size: only "what it already is" can be granted.
    const bool same = r->width == ring_.width() && r->height == ring_.height();
    status = same ? ModeChangeStatus::Unchanged : ModeChangeStatus::Unsupported;
    if (!same)
      WVD_LOGI("mode change to %ux%u refused: %s", r->width, r->height,
               captureDevice_.empty() ? "synthetic source has a fixed size"
                                      : "switched off (--no-mode-change)");
  } else {
    const auto modes = ListDisplayModes(captureDevice_);
    const auto it = std::find_if(modes.begin(), modes.end(), [&](const DisplayModeInfo& m) {
      return m.width == r->width && m.height == r->height && (hz == 0 || m.refreshHz == hz);
    });
    if (it == modes.end()) {
      WVD_LOGW("mode change to %ux%u@%u refused: not offered by %ls", r->width, r->height, hz,
               captureDevice_.c_str());
    } else if (it->current) {
      status = ModeChangeStatus::Unchanged;
    } else {
      DisplayModeInfo before;
      const bool haveBefore = CurrentDisplayMode(captureDevice_, before);
      const LONG rc = ApplyDisplayMode(captureDevice_, it->width, it->height, it->refreshHz);
      if (rc == DISP_CHANGE_SUCCESSFUL) {
        if (!modeChanged_ && haveBefore) {
          originalMode_ = before;
          modeChanged_ = true;
        }
        status = ModeChangeStatus::Applied;
        WVD_LOGI("display mode %ux%u@%u -> %ux%u@%u for the headset (restored at session end)",
                 before.width, before.height, before.refreshHz, it->width, it->height,
                 it->refreshHz);
      } else {
        status = ModeChangeStatus::Failed;
        WVD_LOGW("display mode %ux%u@%u refused by Windows (DISP_CHANGE %ld)", it->width,
                 it->height, it->refreshHz, rc);
      }
    }
  }
  // The capture follows an applied change on its own (ACCESS_LOST → resize →
  // new layout, codec config and mode list); this answers the request now.
  SendDisplayModes(r->requestId, status);
}

void MediaSession::RestoreModeIfChanged() {
  if (!modeChanged_) return;
  modeChanged_ = false;
  LONG rc = RestoreDisplayMode(captureDevice_);
  DisplayModeInfo now;
  if (CurrentDisplayMode(captureDevice_, now) &&
      (now.width != originalMode_.width || now.height != originalMode_.height ||
       now.refreshHz != originalMode_.refreshHz)) {
    // The stored mode is not what the session started with (an earlier
    // dynamic change): put back what this session found.
    rc = ApplyDisplayMode(captureDevice_, originalMode_.width, originalMode_.height,
                          originalMode_.refreshHz);
  }
  if (rc == DISP_CHANGE_SUCCESSFUL)
    WVD_LOGI("display mode restored to %ux%u@%u", originalMode_.width, originalMode_.height,
             originalMode_.refreshHz);
  else
    WVD_LOGW("display mode restore to %ux%u@%u failed (DISP_CHANGE %ld)", originalMode_.width,
             originalMode_.height, originalMode_.refreshHz, rc);
}

// Caller holds encMutex_.
void MediaSession::QueueCodecConfigLocked() {
  if (!encoder_) return;
  PacketWriter w(PacketKind::CodecConfig);
  CodecConfigPayload cc{};
  cc.codecGen = gens_.codecGen.load();
  cc.codec = uint8_t(encoder_->codec());
  cc.hevcProfile = 1;  // Main (SDR reference, §6.7); H.264 streams carry High/Main
  cc.flags = (encoder_->caps().intraRefresh ? 1u : 0u) |
             (encoder_->caps().sliceOutput ? 2u : 0u) | (encoder_->caps().ltr ? 4u : 0u);
  cc.width = encDesc_.width;
  cc.height = encDesc_.height;
  cc.fpsNum = encDesc_.fpsNum;
  cc.fpsDen = encDesc_.fpsDen;
  cc.bitrateKbps = encDesc_.bitrateKbps;
  cc.vui = kSdrReferenceVui;
  cc.csdSize = uint32_t(csd_.size());  // Annex B VPS/SPS/PPS (empty for the test encoder)
  if (depthPacking_) {
    // §3.3: this generation's every picture carries a depth strip; width/
    // height above are the CODED size. The layout rides between the struct
    // and the parameter sets (the QUIC adapter maps it to
    // CodecConfig.embedded_depth).
    cc.reserved |= kCodecConfigEmbeddedDepth;
    w.Append(cc);
    w.Append(depthLayout_);
  } else {
    cc.reserved |= uint8_t(uint32_t(encodedStereoLayout_) << kCodecConfigStereoShift);
    w.Append(cc);
  }
  w.AppendBytes(csd_.data(), csd_.size());
  // §6.6: codec config is reliable *before* any media of that generation. The
  // control queue is drained before media, so this order holds on the wire.
  PushUrgent(w.Take());
  codecConfigPending_ = false;
}

bool MediaSession::ReconfigureEncoder(UINT width, UINT height) {
  std::lock_guard<std::mutex> lk(encMutex_);
  CsGuard cs(encCs_);
  if (!encoder_) return false;
  ConfigureEncodeSizeLocked(width, height);
  // A size change is already a new codec generation: keep packing when it is
  // still wanted, re-derived for the new colour size (§3.3: the layout
  // changing IS a generation change, which this already is).
  EmbeddedDepthPayload layout{};
  if (DepthWantedLocked(layout)) {
    depthPacking_ = true;
    depthLayout_ = layout;
    encDesc_.height = layout.codedHeight;
    encDesc_.bitrateKbps = DepthBitrateLocked();
  } else {
    depthPacking_ = false;
    depthLayout_ = {};
    encDesc_.bitrateKbps = cfg_.bitrateKbps;
  }
  if (RecreateEncoderLocked()) return true;
  if (!depthPacking_) return FallbackToHalfSbsLocked(width, height);
  WVD_LOGW("depth strip unavailable at %ux%u; retrying the colour size",
           encDesc_.width, encDesc_.height);
  depthPacking_ = false;
  depthLayout_ = {};
  encDesc_.height = encColorH_;
  encDesc_.bitrateKbps = cfg_.bitrateKbps;
  return RecreateEncoderLocked();
}

// §3.3 rule 1: pack only while the client advertised embedded_depth, the
// latest StreamSettings.depth_monitors names THIS monitor, and a depth source
// is attached. Never because the capability alone is set.
bool MediaSession::DepthWantedLocked(EmbeddedDepthPayload& layout) {
  layout = {};
  return (depthSrc_ || gameDepthAttached_.load()) && DepthRequested() &&
         DepthLayoutFits(encColorW_, encColorH_, layout);
}

bool MediaSession::DepthRequested() {
  if (cfg_.stereoLayout != msg::StereoLayout::None || !channel_ || !clientDepthCap_.load())
    return false;
  const uint16_t idx = kCaptureMonitorIndex;
  std::lock_guard<std::mutex> lk(depthReqMutex_);
  return std::find(depthMonitorsReq_.begin(), depthMonitorsReq_.end(), idx) !=
         depthMonitorsReq_.end();
}

bool MediaSession::DepthLayoutFits(uint32_t width, uint32_t height,
                                  EmbeddedDepthPayload& layout) const {
  if (!ComputeEmbeddedDepth(width, height, layout)) return false;
  const auto limits = channel_->GetVideoDecodeLimits();
  return (!limits.width || layout.pictureWidth <= limits.width) &&
         (!limits.height || layout.codedHeight <= limits.height) &&
         (!limits.pixelsPerSecond ||
          uint64_t(layout.pictureWidth) * layout.codedHeight <=
              limits.pixelsPerSecond / EffectiveFrameRate());
}

// The strip costs roughly 10% more bits while packing (§4; measured in H1).
uint32_t MediaSession::DepthBitrateLocked() const {
  const uint64_t raised = uint64_t(cfg_.bitrateKbps) * 11 / 10;
  const uint32_t limit = channel_->VideoBitrateLimitKbps();
  return uint32_t((std::min)(raised, uint64_t(limit ? limit : UINT32_MAX)));
}

// Encode thread, encMutex_ held: honour a depth request/capability change as
// a codec generation change (new coded size, CodecConfig with or without the
// layout, IDR). Frame-to-frame gaps stay DEPTH_VALID instead.
void MediaSession::UpdateDepthPackingLocked() {
  EmbeddedDepthPayload layout{};
  const bool want = DepthWantedLocked(layout);
  const bool sameLayout =
      want == depthPacking_ &&
      (!want || (layout.codedHeight == depthLayout_.codedHeight &&
                 layout.mapWidth == depthLayout_.mapWidth &&
                 layout.mapHeight == depthLayout_.mapHeight));
  if (sameLayout) return;
  depthPacking_ = want;
  depthLayout_ = want ? layout : EmbeddedDepthPayload{};
  encDesc_.height = want ? layout.codedHeight : encColorH_;
  encDesc_.bitrateKbps = want ? DepthBitrateLocked() : cfg_.bitrateKbps;
  if (RecreateEncoderLocked()) {
    forceFullFrame_ = true;  // the new generation owes a picture right away
    WVD_LOGI("depth packing %s: colour %ux%u, map %ux%u /%u slices, strip y=%u, coded %ux%u, "
             "target %u kbps (§depth)",
             want ? "ON" : "off", encColorW_, encColorH_, depthLayout_.mapWidth,
             depthLayout_.mapHeight, depthLayout_.slices, depthLayout_.stripY,
             encDesc_.width, encDesc_.height, encDesc_.bitrateKbps);
    return;
  }
  if (want) {
    WVD_LOGE("depth: no encoder for the coded size %ux%u — staying without the strip",
             encDesc_.width, encDesc_.height);
    depthPacking_ = false;
    depthLayout_ = {};
    encDesc_.height = encColorH_;
    encDesc_.bitrateKbps = cfg_.bitrateKbps;
    if (RecreateEncoderLocked()) {
      forceFullFrame_ = true;
      return;
    }
  }
  stopReq_ = true;
}

// Capture thread: feed the depth source for the frame it just captured. The
// layout is read with a TRY on the re-entrant encoder lock — a capture must
// never stall behind an in-flight convert+encode, and skipping a generation
// only costs one frame with DEPTH_VALID clear.
void MediaSession::PostDepthFrame(int64_t qpc, int32_t ptrX, int32_t ptrY) {
  if (!depthSrc_) return;
  uint32_t mapW = 0, mapH = 0, picW = 0, picH = 0;
  if (TryEnterCriticalSection(&encCs_)) {
    if (depthPacking_) {
      mapW = depthLayout_.mapWidth;
      mapH = depthLayout_.mapHeight;
      picW = depthLayout_.pictureWidth;
      picH = depthLayout_.pictureHeight;
    }
    LeaveCriticalSection(&encCs_);
  }
  if (mapW)
    depthSrc_->Generate(mapW, mapH, ptrX, ptrY, picW, picH, qpc);
}

// Capture thread: attach an identity to every published ring frame (see
// FrameIdentity). A short history is enough: AcquireLatest consumes the
// newest, and the encoder is never more than a frame or two behind.
void MediaSession::RecordFrameIdentity(uint64_t ringSeq, uint64_t contentVer,
                                       int64_t qpcCapture, bool refinement,
                                       uint32_t videoEpoch,
                                       std::shared_ptr<const DepthFrame> gameDepth) {
  std::lock_guard<std::mutex> lk(identMutex_);
  idents_.push_back(FrameIdentity{ringSeq, contentVer, qpcCapture, refinement, videoEpoch,
                                  gens_.layoutGen.load(),
                                  std::move(gameDepth)});
  while (idents_.size() > 16) idents_.pop_front();
}

bool MediaSession::FindFrameIdentity(uint64_t ringSeq, FrameIdentity& out) {
  std::lock_guard<std::mutex> lk(identMutex_);
  for (auto it = idents_.rbegin(); it != idents_.rend(); ++it)
    if (it->ringSeq == ringSeq) {
      out = *it;
      return true;
    }
  return false;
}

bool MediaSession::RecreateEncoderLocked() {
  encoder_.reset();  // one hardware session at a time (consumer GPUs cap them)
  std::string name;
  auto next = CreateBestEncoder(encDesc_, &name);
  if (!next) {
    WVD_LOGE("no encoder for %ux%u after reconfiguration", encDesc_.width, encDesc_.height);
    return false;
  }
  encoder_ = std::move(next);
  encoderName_ = name;
  PublishEncodeModeLocked();
  gens_.codecGen.fetch_add(1);
  codecConfigPending_ = true;
  csd_.clear();
  WVD_LOGI("encoder reconfigured: %s %ux%u quality=%s codecGen=%llu",
           encoderName_.c_str(), encDesc_.width, encDesc_.height,
           encDesc_.preferQuality ? "quality" : "speed",
           (unsigned long long)gens_.codecGen.load());
  return true;
}

void MediaSession::PublishEncodeModeLocked() {
  std::lock_guard<std::mutex> lk(encodeModeMutex_);
  encodeMode_ = {encSourceW_, encSourceH_, encColorW_, encColorH_};
}

uint32_t MediaSession::EffectiveFrameRate() const {
  const uint32_t host = cfg_.fps ? cfg_.fps : 60;
  const uint32_t encodeLimit = encodeRateLimit_.load();
  const uint32_t rate = encodeLimit ? encodeLimit : host;
  const uint32_t limit = channel_ ? channel_->VideoFrameRateLimit() : 0;
  return limit ? (std::min)(rate, limit) : rate;
}

// ---------------------------------------------------------------------------
// Capture thread
// ---------------------------------------------------------------------------

bool MediaSession::CaptureDda() {
  uint64_t publishSeq = 0;
  int64_t reinitDelayMs = 100;
  const int64_t startedQpc = QpcNow();
  // Until the first picture is queued, an idle desktop gets a one-shot GDI
  // bootstrap: DDA only reports CHANGES, so a peer connecting to a still
  // desktop would otherwise wait forever for frame #1.
  bool oweFirstPicture = true;

  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  PrecisionSleeper sleeper;
  // Pacing. The stream carries at most `fps` pictures per second whatever the
  // display refreshes at: the encoder's rate control is configured for that
  // rate, and a 120/144 Hz panel would otherwise double the bitrate and the
  // GPU load. After a picture the duplication is left alone until the next
  // one is due; Windows accumulates the changes, so that acquire returns the
  // newest desktop. The 3% slack keeps a display running a hair faster than
  // the nominal rate (60.02 Hz) from losing a frame every few seconds.
  uint32_t lastEpoch = 0;
  uint32_t lastRate = 0;
  bool lastQuality = !channel_ || channel_->PreferVideoQuality();
  int64_t nextPictureQpc = 0;
  int64_t lastActivityQpc = startedQpc;
  bool retainedGameFrame = false;

  while (!stopReq_) {
    const uint32_t epoch = VideoEpoch();
    if (!(epoch & 1u)) {
      if (gameDepth_) gameDepth_->StopRequest();
      if (WaitOnStop(25)) break;
      continue;
    }
    const uint32_t rate = EffectiveFrameRate();
    if (epoch != lastEpoch) {
      lastEpoch = epoch;
      forceFullFrame_ = true;
      nextPictureQpc = 0;
    }
    if (rate != lastRate) {
      lastRate = rate;
      nextPictureQpc = 0;
      // §depth: the strip counts against the headset's decode budget AT THIS
      // RATE — a StreamSettings change that only moves the frame rate must
      // re-run admission, and a still desktop still needs a picture to
      // carry the result (the retained-frame path serves it).
      if (clientDepthCap_.load()) {
        depthEval_.store(true);
        forceFullFrame_ = true;
      }
    }
    const bool quality = !channel_ || channel_->PreferVideoQuality();
    if (quality != lastQuality) {
      lastQuality = quality;
      forceFullFrame_ = true;
    }
    const uint32_t hostRate = cfg_.fps ? cfg_.fps : 60;
    const bool allowSlack = !encodeRateLimit_.load() && rate == hostRate;
    const int64_t pictureInterval = MsToQpc(1000.0 / rate * (allowSlack ? 0.97 : 1.0));
    if (QpcNow() < nextPictureQpc) {
      sleeper.SleepUntilQpc((std::min)(nextPictureQpc, QpcNow() + MsToQpc(20)));
      continue;
    }
    CapturedFrame f;
    std::shared_ptr<const DepthFrame> gameMap;
    bool gameFrame = false;
    if (gameDepth_) {
      const auto plan = ChooseEncodePlan(ring_.width(), ring_.height());
      EmbeddedDepthPayload layout{};
      if (DepthRequested() && gameDepth_->CheckSize(ring_.width(), ring_.height()) &&
          DepthLayoutFits(plan.width, plan.height, layout)) {
        gameFrame = gameDepth_->Capture(outputInfo_.desktopCoords, outputInfo_.adapterLuid,
                                        rate, ring_, f, gameMap);
      } else {
        gameDepth_->StopRequest();
      }
      const int64_t now = QpcNow();
      const bool attached = gameDepth_->Attached(now);
      if (attached != gameDepthAttached_.exchange(attached)) {
        depthEval_.store(true);
        forceFullFrame_ = true;
        WVD_LOGI("game depth source %s", attached ? "attached" : "detached");
      }
      if (!gameFrame && gameDepth_->HasRecentFrame(now)) {
        sleeper.SleepMs(1.0);
        continue;
      }
      if (!gameFrame && retainedGameFrame) {
        oweFirstPicture = true;
        forceFullFrame_ = true;
      }
    }
    // AcquireNextFrame can hold the driver's device lock while waiting. The
    // converter uses the same D3D device, so a blocking wait stalls conversion.
    // Poll without waiting and sleep outside DXGI on an unchanged desktop.
    HRESULT hr;
    if (gameFrame) {
      CapturedFrame cursor;
      if (dda_->Acquire(0, cursor, false, true) == S_OK) f.pointer = std::move(cursor.pointer);
      hr = S_OK;
    } else {
      hr = dda_->Acquire(0, f, forceFullFrame_.load());
    }
    if (hr == S_OK) {
      reinitDelayMs = 100;
      lastActivityQpc = f.qpcCapture;

      // Keep one content counter across desktop and game capture.
      f.contentVer = f.hadDirtyRects ? gens_.contentVer.fetch_add(1) + 1 : gens_.contentVer.load();

      if (f.copied) {
        retainedGameFrame = gameFrame;
        {
          std::lock_guard<std::mutex> lk(statsMutex_);
          ++capFrames_;
          if (lastCaptureQpc_) captureGapMaxMs_ = (std::max)(captureGapMaxMs_, QpcDeltaMs(lastCaptureQpc_, f.qpcCapture));
          lastCaptureQpc_ = f.qpcCapture;
        }
      }

      if (f.copied && depthSrc_) {
        if (f.pointer.positionUpdated) {
          depthPtrX_ = f.pointer.x;
          depthPtrY_ = f.pointer.y;
        }
        const RECT& dc = outputInfo_.desktopCoords;
        PostDepthFrame(f.qpcCapture, depthPtrX_ - dc.left, depthPtrY_ - dc.top);
      }
      if (f.copied) {
        RecordFrameIdentity(++publishSeq, f.contentVer, f.qpcCapture, false, epoch, std::move(gameMap));
        ring_.Publish(f.ringSlot, publishSeq);
      }

      // Cursor packets (shape on change + position) — §6.2.
      std::vector<std::vector<uint8_t>> curPkts;
      cursor_->OnCapturedFrame(f, kCaptureMonitorIndex, curPkts);
      for (auto& p : curPkts) PushUrgent(std::move(p));

      // Encode when content changed, or when a full picture is still owed
      // (first frame after start / after ACCESS_LOST re-create). A static
      // desktop produces no dirty rects at all, so without the forced first
      // frame a peer would connect and never see anything.
      // A pointer-only frame has no image in the ring; an owed picture stays
      // owed and is served from the retained frame below.
      const bool force = f.copied && forceFullFrame_.exchange(false);
      if (f.copied && (f.hadDirtyRects || force)) {
        if (encQueue_.Push(epoch)) {
          oweFirstPicture = false;
          // Anchor the quiet-refinement timer on the picture we just queued.
          lastContentChangeQpc_ = f.qpcCapture;
          refinedContentVer_ = 0;
        }
        // Stay on the existing cadence while pictures keep coming; restart
        // it after a pause so the first change is never held back.
        const bool onCadence = nextPictureQpc && f.qpcCapture - nextPictureQpc < pictureInterval;
        nextPictureQpc = (onCadence ? nextPictureQpc : f.qpcCapture) + pictureInterval;
      }
    } else if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
      // 1 ms polls while the desktop is active (a change is picked up within
      // a millisecond); 4 ms once it has been still for half a second.
      sleeper.SleepMs(QpcNow() - lastActivityQpc > MsToQpc(500) ? 4.0 : 1.0);
      if (stopReq_) break;
      // Static desktop: refinement after T_q on a timer, from the RETAINED
      // source (ring slot), never assumed from re-presentations (§6.4).
      const int64_t now = QpcNow();
      if (oweFirstPicture && now - startedQpc > MsToQpc(400)) {
        UINT bslot = 0;
        if (dda_->CaptureBootstrapFrame(bslot)) {
          const uint64_t cv = gens_.contentVer.fetch_add(1) + 1;
          RecordFrameIdentity(++publishSeq, cv, now, false, epoch);
          ring_.Publish(bslot, publishSeq);
          retainedGameFrame = false;
          lastContentChangeQpc_ = now;
          if (encQueue_.Push(epoch)) {
            oweFirstPicture = false;
            forceFullFrame_ = false;
            WVD_LOGI("queued a GDI bootstrap frame (idle desktop; DDA reports only "
                     "changes) — peer gets picture #1 (§4.1)");
          }
        }
      }
      if (!oweFirstPicture && forceFullFrame_.load()) {
        // A keyframe is owed (recovery request) but the desktop is still:
        // re-encode the retained frame so the IDR actually goes out.
        const uint64_t rcv = gens_.contentVer.load();
        RecordFrameIdentity(++publishSeq, rcv, now, false, epoch);
        if (ring_.RepublishNewest(publishSeq)) {
          forceFullFrame_ = false;
          encQueue_.Push(epoch);
          nextPictureQpc = now + pictureInterval;
        }
      } else if (!oweFirstPicture && lastContentChangeQpc_ &&
                 gens_.contentVer.load() != refinedContentVer_ &&
                 now - lastContentChangeQpc_ > MsToQpc(cfg_.quietMsRefine)) {
        // Refine the newest picture already sitting in the ring (§6.4). It has
        // to be re-offered: the encoder already consumed it once, and
        // AcquireLatest used to report "nothing new", so refinement never ran.
        const uint64_t cv = gens_.contentVer.load();
        RecordFrameIdentity(++publishSeq, cv, now, true, epoch);
        if (ring_.RepublishNewest(publishSeq) && encQueue_.Push(epoch))
          refinedContentVer_ = cv;
      }
    } else {
      // ACCESS_LOST or any other failure: re-create with backoff (§4.1:
      // desktop switch, UAC, mode change).
      WVD_LOGW("capture lost (hr=%s) — re-creating in %lldms", HresultStr(hr).c_str(),
               (long long)reinitDelayMs);
      dda_->Stop();
      cursor_->NoteSourceRecreated();
      if (WaitOnStop((uint32_t)reinitDelayMs)) break;
      reinitDelayMs = (std::min)(reinitDelayMs * 2, int64_t(2000));
      // Both the output object and GDI name can go stale after hotplug or a
      // driver restart. Re-resolve the selected identity before reopening.
      auto freshFactory = CreateDxgiFactory();
      if (!freshFactory) continue;
      factory_ = std::move(freshFactory);
      DxOutputInfo current;
      const auto& selector = cfg_.monitorName.empty() || outputInfo_.monitorDevicePath.empty()
                                 ? cfg_.monitorName : outputInfo_.monitorDevicePath;
      if (!FindOutput(factory_.Get(), selector.c_str(), current)) {
        WVD_LOGW("selected display is unavailable; waiting for it to return");
        continue;
      }
      if (_wcsicmp(current.monitorName.c_str(), outputInfo_.monitorName.c_str()) != 0 ||
          current.adapterLuid.HighPart != outputInfo_.adapterLuid.HighPart ||
          current.adapterLuid.LowPart != outputInfo_.adapterLuid.LowPart) {
        // A new session owns a new capture device, encoder and layout. Do
        // not race their readers by replacing them on this capture thread.
        WVD_LOGI("selected display moved %ls -> %ls; reconnecting capture",
                 outputInfo_.monitorName.c_str(), current.monitorName.c_str());
        stopReq_ = true;
        if (channel_) channel_->Close();
        SetEvent(sendWake_);
        break;
      }
      auto o = FindDxgiOutput(factory_.Get(), outputInfo_.adapterLuid, outputInfo_.monitorName);
      if (!o) continue;
      output_ = o;
      DdaCapture::InitDesc dd{output_, device_, ctx_, nullptr};
      if (FAILED(dda_->Start(dd))) continue;
      // ACCESS_LOST also covers a display MODE change: the surface may now be a
      // different size, which means new ring slots plus a new codec and layout
      // generation (§4.1 generation rules).
      if (dda_->width() != ring_.width() || dda_->height() != ring_.height()) {
        WVD_LOGW("capture resized %ux%u -> %ux%u: rebuilding ring + bumping generations",
                 ring_.width(), ring_.height(), dda_->width(), dda_->height());
        ring_.Deinit();
        if (FAILED(ring_.Init(device_.Get(), dda_->width(), dda_->height(), 3))) break;
        if (!ReconfigureEncoder(dda_->width(), dda_->height())) break;
      }
      // Re-read the layout after a desktop switch or refresh-only mode change.
      outputInfo_.desktopCoords = current.desktopCoords;
      {
        std::lock_guard<std::mutex> lk(layoutMutex_);
        BuildLayoutLocked(dda_->width(), dda_->height());
        gens_.layoutGen.fetch_add(1);
        if (channel_) {
          SendMonitorLayoutLocked();
          SendDisplayModesLocked(0, ModeChangeStatus::None);
        }
      }
      dda_->SetRing(&ring_);
      forceFullFrame_ = true;  // new source → owe a full picture (§4.1)
      oweFirstPicture = true;  // ...and it may have to be bootstrapped again
      refinedContentVer_ = 0;
      reinitDelayMs = 100;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Synthetic capture — CPU test pattern, full-frame (§4.1), pointer moves
// (Separate, §6.2). Exercises ring → NV12 convert → encode → transport with
// no display present (headless bring-up / CI of the P0 plumbing).
// ---------------------------------------------------------------------------

bool MediaSession::CaptureSynthetic() {
  const UINT w = ring_.width(), h = ring_.height();
  std::vector<uint8_t> frame(size_t(w) * h * 4);
  int64_t nextDue = QpcNow();
  uint32_t lastRate = 0;
  uint64_t n = 0;
  PrecisionSleeper sleeper;

  while (!stopReq_) {
    const uint32_t epoch = VideoEpoch();
    if (!(epoch & 1u)) {
      if (WaitOnStop(25)) break;
      nextDue = QpcNow();
      continue;
    }
    const uint32_t rate = EffectiveFrameRate();
    if (rate != lastRate) {
      lastRate = rate;
      nextDue = QpcNow();
      // §depth: a rate-only change moves the decode-budget admission (§4).
      if (clientDepthCap_.load()) depthEval_.store(true);
    }
    const int64_t now = QpcNow();
    if (now < nextDue) {
      sleeper.SleepUntilQpc((std::min)(nextDue, now + MsToQpc(20)));
      continue;
    }
    nextDue = now + MsToQpc(1000.0 / rate);
    // Busy pattern (changes every frame → content_ver advances every frame).
    for (UINT y = 0; y < h; y += 4) {
      for (UINT x = 0; x < w; x += 4) {
        uint32_t c = uint32_t(((x + int(n) * 3) & 0xFF) | ((y * 2 & 0xFF) << 8) |
                              (((x ^ y ^ int(n)) & 0xFF) << 16) | 0xFF000000u);
        for (UINT dy = 0; dy < 4 && y + dy < h; ++dy)
          for (UINT dx = 0; dx < 4 && x + dx < w; ++dx)
            reinterpret_cast<uint32_t*>(frame.data() + size_t(y + dy) * w * 4)[x + dx] = c;
      }
    }

    UINT slot = ring_.AcquireWriteSlot();
    ring_.WriteCpuFrame(slot, frame.data(), w * 4);
    ++n;

    // Synthetic Separate pointer tracing a Lissajous path (§6.2 path).
    CapturedFrame cf;
    cf.contentVer = ++gens_.contentVer;
    cf.hadDirtyRects = true;
    cf.qpcCapture = QpcNow();
    cf.ringSlot = slot;
    cf.pointer.visible = true;
    cf.pointer.positionUpdated = true;
    cf.pointer.x = INT32((w / 2) + (w / 3) * cos(double(n) / 20.0));
    cf.pointer.y = INT32((h / 2) + (h / 3) * sin(double(n) / 13.0));
    if ((n % 90) == 1) {
      // Deliver a small arrow-ish shape once in a while (shape versioning).
      cf.pointer.hasShape = true;
      cf.pointer.shape.width = cf.pointer.shape.height = 16;
      cf.pointer.shape.hotX = cf.pointer.shape.hotY = 0;
      cf.pointer.shape.pixelFormat = 0;
      cf.pointer.shape.pixels.assign(16 * 16 * 4, 0);
      for (int y = 0; y < 16; ++y)
        for (int x = 0; x <= y; ++x) {
          uint8_t* p = cf.pointer.shape.pixels.data() + (size_t(y) * 16 + x) * 4;
          p[0] = p[1] = p[2] = 0xFF;
          p[3] = 0xFF;
        }
      uint64_t hh = 1469598103934665603ull;
      for (uint8_t b : cf.pointer.shape.pixels) { hh ^= b; hh *= 1099511628211ull; }
      cf.pointer.shape.hash = hh ^ (n * 1099511628211ull);
    }

    // Synthetic depth (§depth): the ramp + a square riding the synthetic
    // pointer, paired with this frame's capture stamp.
    PostDepthFrame(cf.qpcCapture, cf.pointer.x, cf.pointer.y);

    std::vector<std::vector<uint8_t>> cur;
    cursor_->OnCapturedFrame(cf, 0, cur);
    for (auto& p : cur) PushUrgent(std::move(p));

    RecordFrameIdentity(n, cf.contentVer, cf.qpcCapture, false, epoch);
    ring_.Publish(slot, n);
    encQueue_.Push(epoch);
    {
      std::lock_guard<std::mutex> lk(statsMutex_);
      ++capFrames_;
      if (lastCaptureQpc_) captureGapMaxMs_ = (std::max)(captureGapMaxMs_, QpcDeltaMs(lastCaptureQpc_, cf.qpcCapture));
      lastCaptureQpc_ = cf.qpcCapture;
    }

  }
  return true;
}

// ---------------------------------------------------------------------------
// Encode thread
// ---------------------------------------------------------------------------

bool MediaSession::ApplyBitrate(uint32_t kbps, const char* why) {
  if (!encoder_) return false;
  const uint32_t from = encDesc_.bitrateKbps;
  const HRESULT hr = encoder_->SetBitrate(kbps);
  if (FAILED(hr)) {
    WVD_LOGI("encoder %s cannot change bitrate while running (hr=%s) — staying at %u kbps",
             encoderName_.c_str(), HresultStr(hr).c_str(), from);
    return false;
  }
  // A re-created or hot-switched encoder starts from the adapted rate.
  encDesc_.bitrateKbps = kbps;
  if (kbps < from || kbps == cfg_.bitrateKbps)
    WVD_LOGI("bitrate %u -> %u kbps (%s)", from, kbps, why);
  else
    WVD_LOGV("bitrate %u -> %u kbps (%s)", from, kbps, why);
  return true;
}

void MediaSession::EncodeLoop() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  PrecisionSleeper sleeper;
  std::vector<uint8_t> nv12buf;
  bool forcedKeyPending = true;  // first picture = random access point (§6.6)
  uint32_t lastEpoch = 0;
  uint64_t lastLayoutGen = 0;

  // Account for both application and MsQuic queues before encoding more.
  // Stale captures are superseded by the newest contents of the capture ring.
  const uint32_t maxKbps = cfg_.bitrateKbps;
  const uint32_t minKbps = (std::min)(
      maxKbps, cfg_.minBitrateKbps ? cfg_.minBitrateKbps : (std::max)(1500u, maxKbps / 8));
  bool canSetBitrate = channel_ != nullptr;
  const bool adapt = cfg_.adaptiveBitrate && channel_ != nullptr;
  uint32_t appliedKbps = maxKbps;
  // Short Wi-Fi pauses and keyframe bursts only skip pictures; the target
  // moves for a link that keeps falling behind (LinkRate).
  LinkRate linkRate(maxKbps, minKbps);
  const int64_t rateClock = QpcNow();
  auto updateRate = [&] {
    if (!canSetBitrate) return;
    std::lock_guard<std::mutex> lk(encMutex_);
    const uint32_t peerLimit = channel_->VideoBitrateLimitKbps();
    const uint64_t target = depthPacking_ ? uint64_t(linkRate.target()) * 11 / 10 : linkRate.target();
    const uint32_t kbps = uint32_t((std::min)(uint64_t(peerLimit ? peerLimit : UINT32_MAX), target));
    if (kbps == appliedKbps) return;
    canSetBitrate = ApplyBitrate(kbps, peerLimit && kbps == peerLimit ?
                                      "headset bitrate limit" : "link queue pressure");
    if (canSetBitrate) appliedKbps = kbps;
  };
  uint64_t lastSubmittedCodecGen = 0;

  while (!stopReq_) {
    auto epoch = encQueue_.Pop(std::chrono::milliseconds(100));
    if (!epoch) {
      // A still desktop sends nothing: the target can still climb back.
      if (adapt) linkRate.OnIdle(QpcDeltaMs(rateClock, QpcNow()));
      updateRate();
      continue;
    }
    if (!(*epoch & 1u) || *epoch != VideoEpoch()) continue;

    if (channel_) {
      const int64_t waitStart = QpcNow();
      const uint32_t stereoRate = encodeRateLimit_.load();
      const uint32_t encoderRate = stereoRate ? stereoRate : (cfg_.fps ? cfg_.fps : 60);
      const size_t transportBudget = (std::max)(size_t(64 * 1024),
          size_t(appliedKbps) * 250 / encoderRate);
      const int64_t frameQpc = MsToQpc(1000.0 / encoderRate);
      const int64_t waitBudget = MsToQpc(double(cfg_.backpressureMs));
      while (!stopReq_) {
        const int64_t elapsed = QpcNow() - waitStart;
        const bool pipelineBehind = elapsed < waitBudget &&
                                    sendQueue_.Size() >= kSendBackpressureDepth;
        const bool transportBehind = elapsed < frameQpc &&
                                     channel_->BacklogBytes() >= transportBudget;
        if (!pipelineBehind && !transportBehind) break;
        sleeper.SleepMs(1.0);
      }
      const int64_t now = QpcNow();
      const bool congested = now - waitStart > frameQpc;
      if (congested) {
        // Pictures queued meanwhile are superseded by the newest one.
        size_t skipped = 1;
        while (auto newer = encQueue_.Pop(std::chrono::milliseconds(0))) {
          epoch = newer;
          ++skipped;
        }
        std::lock_guard<std::mutex> lk(statsMutex_);
        skippedForLink_ += skipped;
      }
      if (adapt) linkRate.OnPicture(QpcDeltaMs(rateClock, now), QpcDeltaMs(waitStart, now));
      updateRate();
      if (stopReq_) break;
      if (congested && channel_->BacklogBytes() >= transportBudget) continue;
    }
    if (*epoch != VideoEpoch()) continue;

    // Embedded depth: a request/capability change becomes a codec
    // generation change (game-depth plan §3.3/§6).
    if (depthEval_.exchange(false)) {
      std::lock_guard<std::mutex> lk(encMutex_);
      CsGuard cs(encCs_);
      UpdateDepthPackingLocked();
      appliedKbps = encDesc_.bitrateKbps;  // a recreated encoder starts at its own target
    }

    // Hold the ring's lifetime guard while using a slot: a mode change on the
    // capture thread re-allocates the slots and must not pull them out from
    // under us.
    FrameRing::SharedGuard guard(ring_);

    UINT slot;
    uint64_t ringSeq;
    if (!ring_.AcquireLatest(slot, ringSeq)) continue;
    FrameRing::ReadSlot readSlot{ring_, slot};

    // The texture, its capture stamp and its depth pair through the identity
    // the capture thread attached to THIS published frame — never through
    // the queue item that woke us: capture can get ahead between Pop and
    // AcquireLatest, which would pair frame B's colour with frame A's depth
    // and mark it DEPTH_VALID.
    FrameIdentity fi;
    if (!FindFrameIdentity(ringSeq, fi) || fi.videoEpoch != VideoEpoch() ||
        fi.layoutGen != gens_.layoutGen.load()) continue;

    if (!converterReady_) continue;

    EncodedAu au;
    bool encSwitchFailed = false;
    bool stereoFellBack = false;
    uint64_t codecGen = 0;
    double convMs = 0;
    bool depthValidPic = false;
    HRESULT hr = S_OK;
    {
      // Convert and encode under one lock: a mode change swaps the encoder
      // (and its picture size) on the capture thread.
      std::lock_guard<std::mutex> lk(encMutex_);
      CsGuard cs(encCs_);  // the capture thread's TryEnter depth-layout read sees this window
      if (!encoder_) break;
      if (fi.videoEpoch != lastEpoch || fi.layoutGen != lastLayoutGen) {
        lastEpoch = fi.videoEpoch;
        lastLayoutGen = fi.layoutGen;
        forcedKeyPending = true;
      }
      const bool quality = !channel_ || channel_->PreferVideoQuality();
      if (quality != encDesc_.preferQuality) {
        encDesc_.preferQuality = quality;
        if (!RecreateEncoderLocked()) {
          stereoFellBack = FallbackToHalfSbsLocked(ring_.width(), ring_.height());
          if (!stereoFellBack) {
            stopReq_ = true;
            break;
          }
        }
        forcedKeyPending = true;
      }

      // Depth for THIS picture: the map paired with its capture stamp. No
      // match means DEPTH_VALID clear and a black strip — never a guess
      // (game-depth plan §6).
      Nv12DepthOverlay overlay{};
      const Nv12DepthOverlay* ovPtr = nullptr;
      // overlay.map points into `normalized`; `df` owns the raw texels the
      // normaliser reads. Both must stay alive until ConvertToTexture has
      // copied the map (its UpdateSubresource runs inside the call) — they
      // are declared here, not inside the depth block below.
      DepthFrame df;
      std::vector<uint8_t> normalized;
      if (depthPacking_) {
        overlay.colorHeight = encColorH_;
        overlay.stripY = depthLayout_.stripY;
        overlay.sliceRows = depthLayout_.sliceRows;
        overlay.slices = depthLayout_.slices;
        overlay.mapWidth = depthLayout_.mapWidth;
        overlay.mapHeight = depthLayout_.mapHeight;
        const DepthFrame* map = fi.gameDepth.get();
        if (!map && depthSrc_ && depthSrc_->Find(fi.qpcCapture, df) && df.valid) map = &df;
        if (map) {
          // Percentile normalisation (§5): raw 16-bit inverse depth →
          // full-range 8-bit strip texels through a smoothed interval.
          const int64_t nowQpc = QpcNow();
          const double dt =
              lastDepthNormQpc_
                  ? QpcDeltaMs(lastDepthNormQpc_, nowQpc) / 1000.0
                  : 1.0 / double(encDesc_.fpsNum ? encDesc_.fpsNum : 60);
          lastDepthNormQpc_ = nowQpc;
          depthNorm_.Observe(map->v.data(), map->v.size(), dt);
          const uint8_t* lut = depthNorm_.Lut();
          NormalizeDepthMap(*map, depthLayout_.mapWidth, depthLayout_.mapHeight, lut, normalized);
          overlay.map = normalized.data();
          depthValidPic = true;
        }
        ovPtr = &overlay;  // map == nullptr → the pass writes a black strip
      }

      // BGRA ring slot → NV12 texture at the encode size, on the GPU. Only an
      // encoder without texture input asks for the CPU copy. With a depth
      // overlay the target is the taller CODED texture (colour + guard +
      // strip + padding).
      const int64_t convT0 = QpcNow();
      ID3D11Texture2D* nv12Tex = nullptr;
      hr = converter_.ConvertToTexture(ring_.SlotSrv(slot), ring_.width(), ring_.height(),
                                       encDesc_.width, encDesc_.height, cfg_.chroma, &nv12Tex,
                                       ovPtr);
      convMs = QpcDeltaMs(convT0, QpcNow());
      if (FAILED(hr)) {
        WVD_LOGW("NV12 convert failed hr=%s", HresultStr(hr).c_str());
        continue;
      }

      SourcePicture sp;
      sp.texture = nv12Tex;
      sp.readCpu = [&](const uint8_t** data, size_t* size) -> HRESULT {
        const HRESULT r = converter_.Readback(nv12Tex, nv12buf);
        if (FAILED(r)) return r;
        *data = nv12buf.data();
        *size = nv12buf.size();
        return S_OK;
      };
      sp.contentVer = fi.contentVer;
      sp.qpcCapture = fi.qpcCapture;
      sp.layoutGen = fi.layoutGen;
      sp.videoEpoch = fi.videoEpoch;
      sp.qualityBoost = fi.refinement;
      sp.depthValid = depthValidPic;  // travels with the picture through a
                                      // pipelined encoder to its own header

      const uint64_t currentCodecGen = gens_.codecGen.load();
      if (currentCodecGen != lastSubmittedCodecGen) {
        lastSubmittedCodecGen = currentCodecGen;
        forcedKeyPending = true;
      }
      if (forcedKeyPending) encoder_->ForceKeyframe();
      forcedKeyPending = false;
      hr = encoder_->Encode(sp, au);
      if (FAILED(hr)) {
        // Hot-switch to next tier inside the session (§4.6): new encoder →
        // new codec_gen → IDR; connection stays up.
        WVD_LOGE("encoder %s failed hr=%s — hot-switch (§4.6)", encoderName_.c_str(),
                 HresultStr(hr).c_str());
        const std::string failed = encoderName_;
        encoder_.reset();
        auto next = CreateNextTier(failed, encDesc_, &encoderName_);
        if (!next) {
          stereoFellBack = FallbackToHalfSbsLocked(ring_.width(), ring_.height());
          encSwitchFailed = !stereoFellBack;
        } else {
          encoder_ = std::move(next);
          gens_.codecGen.fetch_add(1);
          codecConfigPending_ = true;
          csd_.clear();
          forcedKeyPending = true;
        }
        au.bytes.clear();
      } else if (!au.bytes.empty() && (au.videoEpoch != VideoEpoch() ||
                                     au.layoutGen != gens_.layoutGen.load() ||
                                     (codecConfigPending_ && !au.key))) {
        au.bytes.clear();
        forcedKeyPending = true;
        forceFullFrame_ = true;
      } else if (!au.bytes.empty() && encoder_->vendor() != Vendor::Test) {
        // Parameter sets: keep the newest, and make every keyframe
        // self-contained so a recovery IDR never depends on an old config.
        const Codec codec = encoder_->codec();
        if (au.key) {
          auto ps = ExtractParameterSets(codec, au.bytes.data(), au.bytes.size());
          if (!ps.empty()) {
            if (ps != csd_) {
              csd_ = std::move(ps);
              // The QUIC receiver gets decoder parameter sets in CodecConfig.
              if (!codecConfigPending_) {
                gens_.codecGen.fetch_add(1);
                codecConfigPending_ = true;
              }
            }
          } else if (!csd_.empty()) {
            au.bytes.insert(au.bytes.begin(), csd_.begin(), csd_.end());
          }
        }
        if (csd_.empty()) csd_ = encoder_->codecPrivate();
      }
      if (!au.bytes.empty() && codecConfigPending_) QueueCodecConfigLocked();
      if (!au.bytes.empty() && au.depthValid) ++depthValidFrames_;
      codecGen = gens_.codecGen.load();
    }
    if (stereoFellBack && channel_) SendDisplayModes(0, ModeChangeStatus::None);
    {
      std::lock_guard<std::mutex> lk(statsMutex_);
      convTimesMs_.push_back(convMs);
      if (convTimesMs_.size() > 600)
        convTimesMs_.erase(convTimesMs_.begin(), convTimesMs_.begin() + 300);
    }
    if (encSwitchFailed) {
      WVD_LOGE("no encoder tier left — ending the session");
      stopReq_ = true;
      break;
    }
    if (au.bytes.empty()) continue;  // pipelined encoder / switched tiers

    const uint64_t picSeq = gens_.picSeq.fetch_add(1) + 1;
    if (au.key) {  // recovery coalescing: the newest keyframe on its way out
      lastKeyPicSeq_ = uint32_t(picSeq);
      lastKeyQpc_ = QpcNow();
      keyOwedSinceQpc_ = 0;
    }

    // Media packet (§7 header with every generation).
    PacketWriter w(PacketKind::Media, picSeq);
    MediaPayloadHeader mh{};
    mh.sessionGen = gens_.sessionGen.load();
    mh.layoutGen = au.layoutGen;
    mh.codecGen = codecGen;
    mh.picSeq = uint32_t(picSeq);
    // Identity comes from the OUTPUT picture: an async encoder can hand back
    // an earlier frame than the one just submitted, and the header must
    // describe what these bytes decode to. Fallback only when the encoder
    // published no identity at all.
    const int64_t picQpc = au.qpcCapture ? au.qpcCapture : fi.qpcCapture;
    mh.contentVer = uint32_t(au.contentVer ? au.contentVer : fi.contentVer);
    mh.deadlineQpc = picQpc + MsToQpc(cfg_.deadlineMs);
    mh.monitor = kCaptureMonitorIndex;
    mh.slice = 0;
    mh.sliceCount = 1;  // P0: one unit per picture (§6.6 tile rules later)
    mh.flags = uint16_t((au.key ? 1u : 0u) | (au.refinement ? 2u : 0u) | 4u /*lastSlice*/ |
                        (au.depthValid ? kMediaDepthValid : 0));
    mh.dataSize = uint32_t(au.bytes.size());
    mh.videoEpoch = au.videoEpoch;
    w.Append(mh);
    w.AppendBytes(au.bytes.data(), au.bytes.size());

    SendWork sw;
    sw.framed = w.Take();
    sw.deadlineQpc = mh.deadlineQpc;
    PushMedia(std::move(sw));
    {
      std::lock_guard<std::mutex> lk(statsMutex_);
      ++encFrames_;
    }
    RecordStageTimes(au);
  }
}

// ---------------------------------------------------------------------------
// Send thread — control, then cursor, then ordered encoded media
// ---------------------------------------------------------------------------

void MediaSession::SendLoop() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  int64_t lastHeartbeat = 0;
  auto send = [&](const std::vector<uint8_t>& pkt) {
    if (channel_->Send(pkt.data(), pkt.size())) return true;
    WVD_LOGW("channel closed by peer");
    stopReq_ = true;
    return false;
  };
  while (!stopReq_ && channel_) {
    while (auto pkt = controlQueue_.Pop(std::chrono::milliseconds(0)))
      if (!send(*pkt)) return;
    while (auto pkt = cursorQueue_.Pop(std::chrono::milliseconds(0)))
      if (!send(*pkt)) return;
    // A congested channel must still send video even while audio keeps arriving.
    for (int i = 0; i < 4; ++i) {
      auto pkt = audioQueue_.Pop(std::chrono::milliseconds(0));
      if (!pkt) break;
      if (!send(*pkt)) return;
    }

    // Heartbeat every 1 s (clock sync, §8).
    int64_t now = QpcNow();
    if (now - lastHeartbeat > MsToQpc(1000)) {
      lastHeartbeat = now;
      PacketWriter w(PacketKind::Heartbeat);
      HeartbeatPayload hb{gens_.sessionGen.load(), now};
      w.Append(hb);
      if (!send(w.Take())) return;
    }

    // Encoded media must retain reference order.
    auto sw = sendQueue_.Pop(std::chrono::milliseconds(0));
    if (!sw) {
      // Woken by any enqueue; the timeout only paces the heartbeat.
      WaitForSingleObject(sendWake_, 50);
      continue;
    }
    // Keep encoder reference order here. The transport expires unsent
    // shards when their presentation budget is gone and supports recovery.
    const auto sendStarted = QpcNow();
    if (!send(sw->framed)) return;
    const auto sendFinished = QpcNow();
    {
      std::lock_guard<std::mutex> lk(statsMutex_);
      ++sentFrames_;
      bytesSent_ += sw->framed.size();
      if (lastSendQpc_) sendGapMaxMs_ = (std::max)(sendGapMaxMs_, QpcDeltaMs(lastSendQpc_, sendFinished));
      lastSendQpc_ = sendFinished;
      sendTimesMs_.push_back(QpcDeltaMs(sendStarted, sendFinished));
      if (sendTimesMs_.size() > 600) sendTimesMs_.erase(sendTimesMs_.begin(), sendTimesMs_.begin() + 300);
      if (QpcNow() > sw->deadlineQpc) ++missedUpdates_;
    }
  }
}

// ---------------------------------------------------------------------------
// Recv thread — Hello, RecoveryRequest, Input (§7), InputRelease (§4.4)
// ---------------------------------------------------------------------------

void MediaSession::RecvLoop() {
  while (!stopReq_ && channel_) {
    auto bytes = channel_->RecvPacket(200);
    if (!bytes) {
      // Stop spinning on a dead socket; the host will re-accept (§4.2).
      if (!channel_->IsConnected()) {
        WVD_LOGW("peer disconnected — session will end for reconnect (§4.2)");
        stopReq_ = true;
        SetEvent(sendWake_);
      }
      continue;
    }
    auto pv = PacketView::Parse(bytes->data(), bytes->size());
    if (!pv) {
      WVD_LOGW("bad packet from peer");
      continue;
    }
    switch (pv->kind()) {
      case PacketKind::Hello: {
        if (auto h = pv->Get<HelloPayload>()) {
          char name[sizeof(h->deviceName) + 1] = {};
          memcpy(name, h->deviceName, sizeof(h->deviceName));  // not NUL-terminated on the wire
          WVD_LOGI("peer hello: proto %u mpps<=%u av1=%u thermal=%u audio=%u depth=%u name='%s'",
                   h->protocolVersion, h->maxDecodedPixelsPerSec / 1000000, h->wantsAv1,
                   h->thermalState, (h->reserved & kHelloWantsAudio) ? 1u : 0u,
                   (h->reserved & kHelloEmbeddedDepth) ? 1u : 0u, name);
          SetAudioWanted((h->reserved & kHelloWantsAudio) != 0);
          // Capability only — never a request (game-depth plan §3.3).
          const bool cap = (h->reserved & kHelloEmbeddedDepth) != 0;
          if (cap != clientDepthCap_.exchange(cap)) {
            WVD_LOGI("depth: headset %s embedded depth (DecoderCaps.embedded_depth)",
                     cap ? "can unpack" : "no longer advertises");
            // Re-evaluate even on a still desktop: owe a picture so the new
            // codec generation's IDR actually goes out (the capture thread's
            // retained-frame path serves forceFullFrame_ when idle).
            depthEval_.store(true);
            forceFullFrame_ = true;
          }
        }
        break;
      }
      case PacketKind::DepthMonitors: {
        // StreamSettings.depth_monitors: the full current list on every
        // message (indices into our MonitorLayout).
        if (auto d = pv->Get<DepthMonitorsPayload>()) {
          auto ids = pv->ArrayAt<uint16_t>(sizeof(DepthMonitorsPayload), d->count);
          if (ids.size() == d->count) {
            size_t n = 0;
            bool changed = false;
            {
              std::lock_guard<std::mutex> lk(depthReqMutex_);
              changed = ids != depthMonitorsReq_;
              depthMonitorsReq_ = std::move(ids);
              n = depthMonitorsReq_.size();
            }
            if (changed) {
              WVD_LOGI("depth: headset requests game depth on %zu monitor(s) "
                       "(StreamSettings.depth_monitors)",
                       n);
              depthEval_.store(true);
              forceFullFrame_ = true;  // a still desktop must not stall the toggle
            }
          }
        }
        break;
      }
      case PacketKind::RecoveryRequest:
        HandleRecovery(*pv);
        break;
      case PacketKind::ModeChange:
        HandleModeChange(*pv);
        break;
      case PacketKind::Input:
        ForwardInputToHelper(*pv);
        break;
      case PacketKind::InputRelease: {
        if (auto r = pv->Get<InputReleasePayload>()) {
          WVD_LOGW("InputRelease gen=%llu reason=%u (§4.4)",
                   (unsigned long long)r->inputGen, r->reason);
          if (inputArmed_) InputRequest(InputReleaseAll, &*r, sizeof(*r), nullptr);
        }
        break;
      }
      default:
        break;  // stats/heartbeat from headset etc. — Phase 1
    }
  }
}

// ---------------------------------------------------------------------------
// Audio (WASAPI loopback -> PacketKind::Audio), only while the headset asks
// ---------------------------------------------------------------------------

void MediaSession::SetAudioWanted(bool wanted) {
  if (wanted && !cfg_.allowAudio) {
    if (!audioRefusedLogged_) WVD_LOGI("audio: the headset asked for sound; off on this PC (--no-audio)");
    audioRefusedLogged_ = true;
    return;
  }
  if (wanted == audioOn_) return;
  audioOn_ = wanted;
  if (!wanted) {
    // Its windows are hidden or sound is off on the headset: stop capturing.
    if (audio_) audio_->Stop();
    WVD_LOGI("audio: off (headset)");
    return;
  }
  if (!audio_) audio_ = std::make_unique<LoopbackCapture>();
  audio_->Start([this](const int16_t* stereo, uint32_t frames, uint32_t rate) {
    if (stopReq_) return;
    const uint64_t seq = ++audioSeq_;
    PacketWriter w(PacketKind::Audio, seq);
    AudioPayload a{};
    a.sessionGen = gens_.sessionGen.load();
    a.seq = seq;
    a.qpcCapture = QpcNow();
    a.sampleRate = rate;
    a.channels = 2;
    a.format = 1;
    a.frames = frames;
    w.Append(a);
    w.AppendBytes(stereo, static_cast<size_t>(frames) * 2 * sizeof(int16_t));
    audioQueue_.Push(w.Take());
    SetEvent(sendWake_);
  });
}

void MediaSession::HandleRecovery(PacketView pv) {
  auto r = pv.Get<RecoveryRequestPayload>();
  if (!r) return;
  std::lock_guard<std::mutex> lk(encMutex_);
  // The app requests an IDR with generation zero before it has a CodecConfig.
  if (r->codecGen && r->codecGen != gens_.codecGen.load()) {
    WVD_LOGV("recovery for stale codecGen — ignored (§6.6)");
    return;
  }
  if (!encoder_) return;
  const int64_t now = QpcNow();
  // A forced keyframe not yet produced already answers any request, including
  // the transport's own (refPicSeq 0: it dropped a picture locally). Stale
  // after 500 ms in case the encoder never honoured it.
  const int64_t owedSince = keyOwedSinceQpc_.load();
  if (owedSince && QpcDeltaMs(owedSince, now) < 500.0) {
    WVD_LOGV("recovery: kind=%u — a forced IDR is already pending", r->kind);
    return;
  }
  // The headset names the last picture it decoded. A keyframe sent after it
  // is still in flight unless it is older than WIRE.md §8's 150 ms: then it
  // was lost too and a new one is due.
  constexpr double kKeyInFlightMs = 150.0;
  const uint32_t lastKey = lastKeyPicSeq_.load();
  const double keyAgeMs = QpcDeltaMs(lastKeyQpc_.load(), now);
  if (r->refPicSeq != 0 && lastKey != 0 && int32_t(lastKey - r->refPicSeq) > 0 &&
      keyAgeMs < kKeyInFlightMs) {
    WVD_LOGI("recovery: kind=%u — IDR #%u (after last good #%u) sent %.0f ms ago, not forcing another",
             r->kind, lastKey, r->refPicSeq, keyAgeMs);
    return;
  }
  // §6.6: try reference invalidation / LTR first (module decides), else IDR;
  // P0 modules implement IDR + intra-refresh; LTR/refval negotiation Phase 1.
  encoder_->ForceKeyframe();
  keyOwedSinceQpc_ = now;
  forceFullFrame_ = true;  // a still desktop still needs a picture to carry the IDR
  WVD_LOGI("recovery: kind=%u → forced IDR, next picture is a RAP", r->kind);
}

// ---------------------------------------------------------------------------
// Input helper (§4.4 / §4.7)
// ---------------------------------------------------------------------------

bool MediaSession::InputRequest(uint32_t cmd, const void* extra, size_t extraLen,
                                IpcStatusResponse* stOut) {
  IpcHeader hdr{};
  hdr.cmd = cmd;
  hdr.flags = 1;
  hdr.callerAuth = cfg_.inputToken;
  PacketWriter w(PacketKind::IpcCommand);
  w.Append(hdr);
  if (extra && extraLen) w.AppendBytes(extra, extraLen);
  std::vector<uint8_t> resp;
  if (!inputPipe_.Request(w.Take(), resp, 250)) return false;
  auto pv = PacketView::Parse(resp.data(), resp.size());
  if (!pv) return false;
  auto st = pv->Get<IpcStatusResponse>();
  if (!st) return false;
  if (stOut) *stOut = *st;
  return true;
}

bool MediaSession::EnsureInputArmed() {
  if (cfg_.inputPipe.empty() || cfg_.inputToken == 0) return false;
  if (inputRevoked_ || inputTokenRejected_) return false;
  if (inputArmed_ && inputPipe_.open()) return true;
  const ULONGLONG now = GetTickCount64();
  if (lastInputConnectTry_ && now - lastInputConnectTry_ < 2000) return false;
  lastInputConnectTry_ = now;
  inputArmed_ = false;
  if (!inputPipe_.open() && !inputPipe_.Open(cfg_.inputPipe.c_str(), 200)) return false;

  // The token IS the operator's consent (until the tray UI exists, §4.7).
  IpcStatusResponse st{};
  if (!InputRequest(InputArm, nullptr, 0, &st)) {
    WVD_LOGW("InputArm got no answer — input disabled for now");
    return false;
  }
  if (st.detail & 0x4) {
    WVD_LOGE("input helper rejected the arming token — check --input-token");
    inputTokenRejected_ = true;
    return false;
  }
  helperInputGen_ = st.inputGen;
  helperInstance_ = st.layoutGen;
  inputArmed_ = st.state == 1;
  if (inputArmed_)
    WVD_LOGI("input armed with the supplied token (helper inputGen=%llu, §4.7 consent)",
             (unsigned long long)helperInputGen_);
  return inputArmed_;
}

void MediaSession::ReleaseAndDisarmInput() {
  if (!inputArmed_) return;
  InputReleasePayload r{};
  r.inputGen = helperInputGen_;
  r.reason = kReleaseDisconnect;
  InputRequest(InputReleaseAll, &r, sizeof(r), nullptr);
  InputRequest(InputDisarm, nullptr, 0, nullptr);  // arming is per session
  inputArmed_ = false;
  inputPipe_.Close();
  WVD_LOGI("input released + disarmed (session end, §4.4)");
}

// §4.4 coordinate transform: monitor pixels → virtual desktop, then hand a
// typed, validated event batch to the privileged injector over the pipe.
void MediaSession::ForwardInputToHelper(PacketView pv) {
  auto hdr = pv.Get<InputPayload>();
  if (!hdr) return;
  if (!EnsureInputArmed()) return;

  auto events = pv.ArrayAt<InputEvent>(sizeof(InputPayload), hdr->count);
  if (events.size() != hdr->count || hdr->count > 256) {
    WVD_LOGW("input: truncated/oversized event array — rejected");
    return;
  }

  // Validate against the CURRENT layout & bounds, then transform. An event
  // naming an unknown monitor is DROPPED outright: forwarding it with raw,
  // un-normalised coordinates would drive the real cursor to the wrong place.
  std::vector<InputEvent> kept;
  kept.reserve(events.size());
  std::unique_lock<std::mutex> layoutLock(layoutMutex_);
  for (auto ev : events) {
    const auto kind = InputKind(ev.kind);
    const bool positional = kind == InputKind::MoveAbs || kind == InputKind::Button ||
                            kind == InputKind::Touch || kind == InputKind::Pen;
    if (positional) {
      if (ev.monitor >= monitors_.size()) {
        WVD_LOGW("input: monitor index %u out of range — event dropped (§4.4)", ev.monitor);
        continue;
      }
      const MonitorDesc& m = monitors_[ev.monitor];
      if (m.width < 2 || m.height < 2) continue;
      const float u = float(ev.x) / float(m.width - 1);
      const float v = float(ev.y) / float(m.height - 1);
      if (u < 0.f || u > 1.f || v < 0.f || v > 1.f) {
        WVD_LOGV("input: uv (%.3f,%.3f) outside monitor %u — event dropped", u, v,
                 ev.monitor);
        continue;
      }
      auto d = layout_.UvToDesktop(ev.monitor, u, v);
      if (!d) continue;
      ev.x = d->first;
      ev.y = d->second;
      ev.monitor = 0xFFFF;  // marks "already desktop coords"
    }
    kept.push_back(ev);
  }
  layoutLock.unlock();
  if (kept.empty()) return;  // nothing valid — do not bother the helper

  // The helper owns the input generation; stamp ours from its InputArm reply so
  // a helper-side disarm (kill switch) makes every older batch stale.
  InputPayload outHdr = *hdr;
  outHdr.inputGen = helperInputGen_;
  outHdr.count = uint32_t(kept.size());
  std::vector<uint8_t> body(sizeof(outHdr) + kept.size() * sizeof(InputEvent));
  memcpy(body.data(), &outHdr, sizeof(outHdr));
  memcpy(body.data() + sizeof(outHdr), kept.data(), kept.size() * sizeof(InputEvent));

  IpcStatusResponse st{};
  if (!InputRequest(InputInject, body.data(), body.size(), &st)) {
    // Fail closed: input must never silently queue up behind a dead helper.
    WVD_LOGW("input helper unreachable — dropping input batch (§4.7)");
    inputArmed_ = false;
    inputPipe_.Close();
    return;
  }
  if (st.detail & 0x2) {
    if (st.layoutGen && st.layoutGen != helperInstance_) {
      // PipeClient may have transparently reconnected to a restarted helper.
      // This is different from revocation by the current helper.
      inputArmed_ = false;
      inputPipe_.Close();
      lastInputConnectTry_ = 0;
      return;
    }
    if (st.state == 0) {
      // Disarmed on the PC (kill switch / tray). Do not re-arm ourselves.
      WVD_LOGW("input disarmed by the helper — forwarding stops for this session");
      inputRevoked_ = true;
      inputArmed_ = false;
    } else {
      // Suspended (locked session) — keep the connection; batches are dropped.
      WVD_LOGV("input suspended by the helper (session locked?)");
    }
  }
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------

void MediaSession::RecordStageTimes(const EncodedAu& au) {
  std::lock_guard<std::mutex> lk(statsMutex_);
  encTimesMs_.push_back(QpcDeltaMs(au.qpcEncodeIn, au.qpcEncodeOut));
  if (encTimesMs_.size() > 600) encTimesMs_.erase(encTimesMs_.begin(), encTimesMs_.begin() + 300);
}

static double Percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  size_t i = size_t(p * (v.size() - 1) + 0.5);
  return v[i];
}

MediaSession::Snapshot MediaSession::GetSnapshot() {
  Snapshot s{};
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  double dtSec = double(now.QuadPart - lastStatsTick_.QuadPart) / double(freq.QuadPart);
  if (dtSec <= 0) dtSec = 1e-6;
  {
    std::lock_guard<std::mutex> lk(statsMutex_);
    s.framesCaptured = capFrames_;
    s.framesEncoded = encFrames_;
    s.framesSent = sentFrames_;
    s.framesDroppedLate = lateDrops_;
    s.missedUpdates = missedUpdates_;
    s.skippedForLink = skippedForLink_;
    s.encP50Ms = Percentile(encTimesMs_, 0.50);
    s.encP95Ms = Percentile(encTimesMs_, 0.95);
    s.convP50Ms = Percentile(convTimesMs_, 0.50);
    s.convP95Ms = Percentile(convTimesMs_, 0.95);
    s.captureGapMaxMs = captureGapMaxMs_;
    s.sendGapMaxMs = sendGapMaxMs_;
    s.sendP95Ms = Percentile(sendTimesMs_, 0.95);
    captureGapMaxMs_ = sendGapMaxMs_ = 0;
    sendTimesMs_.clear();
    s.kbpsOut = uint32_t((bytesSent_ - lastBytes_) * 8 / dtSec / 1000.0);
    s.fpsOut = uint32_t((sentFrames_ - lastSentFrames_) / dtSec);
    lastStatsTick_ = now;
    lastSentFrames_ = sentFrames_;
    lastBytes_ = bytesSent_;
  }
  s.encQueue = uint32_t(encQueue_.GetStats().depth);
  s.sendQueue = uint32_t(sendQueue_.GetStats().depth);
  s.capQueue = uint32_t(ring_.GetStats().depth);
  s.pointerState = cursor_->lastState();
  {
    std::lock_guard<std::mutex> lk(encMutex_);
    s.encoder = encoderName_;
    s.targetKbps = encDesc_.bitrateKbps;
    s.encWidth = encDesc_.width;
    s.encHeight = encDesc_.height;
    s.zeroCopy = encoder_ && encoder_->zeroCopy();
    s.depthPacking = depthPacking_;
    s.depthMapWidth = depthLayout_.mapWidth;
    s.depthMapHeight = depthLayout_.mapHeight;
    s.depthValidFrames = depthValidFrames_;
  }
  return s;
}

}  // namespace wvd

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

MediaSession::MediaSession(SessionConfig cfg, GenCounters& gens)
    : cfg_(std::move(cfg)), gens_(gens), cursor_(std::make_unique<CursorSource>(gens)) {
  sendWake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

MediaSession::~MediaSession() {
  Stop();
  if (sendWake_) CloseHandle(sendWake_);
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
  FitEncodeSize(w, h, cfg_.maxEncodePixels, encDesc_.width, encDesc_.height);
  if (encDesc_.width != w || encDesc_.height != h)
    WVD_LOGI("encode size %ux%u for a %ux%u source (pixel budget %llu)", encDesc_.width,
             encDesc_.height, w, h, (unsigned long long)cfg_.maxEncodePixels);
  encDesc_.fpsNum = cfg_.fps ? cfg_.fps : 60;
  encDesc_.fpsDen = 1;
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

  thCapture_ = std::thread([this] { synthetic ? CaptureSynthetic() : CaptureDda(); });
  thEncode_ = std::thread([this] { EncodeLoop(); });
  thSend_ = std::thread([this] { SendLoop(); });
  if (channel_) thRecv_ = std::thread([this] { RecvLoop(); });
  WVD_LOGI("media session started: source %ux%u, encode %ux%u@%u enc=%s input=%s "
           "sessionGen=%llu",
           w, h, encDesc_.width, encDesc_.height, encDesc_.fpsNum, encoderName_.c_str(),
           encoder_->zeroCopy() ? "gpu-texture" : "cpu-copy",
           (unsigned long long)gens_.sessionGen.load());
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
  std::vector<MonitorDesc> list;
  uint16_t captureIndex = 0;
  if (!synthetic) {
    // A mode change leaves the factory stale (IsCurrent false): its outputs
    // would still report the old desktop rectangles.
    if (factory_ && !factory_->IsCurrent()) factory_ = CreateDxgiFactory();
    if (factory_) {
      for (auto& o : EnumerateOutputs(factory_.Get())) {
        MonitorDesc m{};
        auto u = WideToUtf8(o.monitorName.c_str());
        memcpy(m.id, u.data(), (std::min)(u.size(), sizeof(m.id) - 1));
        m.x = o.desktopCoords.left;
        m.y = o.desktopCoords.top;
        m.width = UINT(o.desktopCoords.right - o.desktopCoords.left);
        m.height = UINT(o.desktopCoords.bottom - o.desktopCoords.top);
        POINT dpi = MonitorDpi(o.desktopCoords);
        m.dpiX = UINT(dpi.x);
        m.dpiY = UINT(dpi.y);
        m.primary = (m.x == 0 && m.y == 0) ? 1 : 0;
        const bool captured =
            _wcsicmp(o.monitorName.c_str(), outputInfo_.monitorName.c_str()) == 0;
        if (captured) {
          // The captured monitor reports the captured surface size (the
          // duplication's), not a possibly scaled desktop rect.
          m.width = w;
          m.height = h;
        }
        list.push_back(m);
        if (captured) captureIndex = uint16_t(list.size() - 1);
      }
    }
  }
  if (list.empty()) {
    // Synthetic / headless: one virtual monitor at the ring size.
    MonitorDesc m{};
    strcpy_s(m.id, synthetic ? "SYNTH0" : "WVD0");
    m.width = w;
    m.height = h;
    m.dpiX = m.dpiY = 96;
    m.primary = 1;
    list.push_back(m);
    captureIndex = 0;
  }
  monitors_ = std::move(list);
  captureMonitorIndex_ = captureIndex;
  layout_.Update(monitors_);
  WVD_LOGI("layout: %zu monitor(s); capturing index %u (%s) %ux%u", monitors_.size(),
           captureIndex, monitors_[captureIndex].id, w, h);
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
  const uint16_t index = captureMonitorIndex_.load();
  auto describe = [&](uint32_t mw, uint32_t mh, uint32_t hz, bool current, bool native) {
    uint32_t flags = (current ? kModeCurrent : 0u) | (native ? kModeNative : 0u);
    if (cfg_.maxEncodePixels && uint64_t(mw) * mh > cfg_.maxEncodePixels)
      flags |= kModeScaledForEncode;
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
  if (r->monitor != captureMonitorIndex_.load()) {
    WVD_LOGW("mode change for monitor %u, but this session streams monitor %u — refused",
             r->monitor, captureMonitorIndex_.load());
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
  w.Append(cc);
  w.AppendBytes(csd_.data(), csd_.size());
  // §6.6: codec config is reliable *before* any media of that generation. The
  // control queue is drained before media, so this order holds on the wire.
  PushUrgent(w.Take());
  codecConfigPending_ = false;
}

bool MediaSession::ReconfigureEncoder(UINT width, UINT height) {
  std::lock_guard<std::mutex> lk(encMutex_);
  if (!encoder_) return false;
  FitEncodeSize(width, height, cfg_.maxEncodePixels, encDesc_.width, encDesc_.height);
  encoder_.reset();  // one hardware session at a time (consumer GPUs cap them)
  std::string name;
  auto next = CreateBestEncoder(encDesc_, &name);
  if (!next) {
    WVD_LOGE("no encoder for %ux%u after mode change", width, height);
    return false;
  }
  encoder_ = std::move(next);
  encoderName_ = name;
  gens_.codecGen.fetch_add(1);
  codecConfigPending_ = true;
  csd_.clear();
  WVD_LOGI("encoder reconfigured: %s %ux%u (source %ux%u) codecGen=%llu",
           encoderName_.c_str(), encDesc_.width, encDesc_.height, width, height,
           (unsigned long long)gens_.codecGen.load());
  return true;
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
  const int64_t pictureInterval = MsToQpc(1000.0 / double(encDesc_.fpsNum) * 0.97);
  int64_t nextPictureQpc = 0;
  int64_t lastActivityQpc = startedQpc;

  while (!stopReq_) {
    if (QpcNow() < nextPictureQpc) {
      sleeper.SleepUntilQpc(nextPictureQpc);
      continue;
    }
    CapturedFrame f;
    // AcquireNextFrame can hold the driver's device lock while waiting. The
    // converter uses the same D3D device, so a blocking wait stalls conversion.
    // Poll without waiting and sleep outside DXGI on an unchanged desktop.
    HRESULT hr = dda_->Acquire(0, f, forceFullFrame_.load());
    if (hr == S_OK) {
      reinitDelayMs = 100;
      lastActivityQpc = f.qpcCapture;

      // Mirror the source content counter so the refinement timer and every
      // generation-aware consumer see the same value the frames carry.
      gens_.contentVer.store(f.contentVer);

      if (f.copied) {
        {
          std::lock_guard<std::mutex> lk(statsMutex_);
          ++capFrames_;
          if (lastCaptureQpc_) captureGapMaxMs_ = (std::max)(captureGapMaxMs_, QpcDeltaMs(lastCaptureQpc_, f.qpcCapture));
          lastCaptureQpc_ = f.qpcCapture;
        }
        ring_.Publish(f.ringSlot, ++publishSeq);
      }

      // Cursor packets (shape on change + position) — §6.2.
      std::vector<std::vector<uint8_t>> curPkts;
      cursor_->OnCapturedFrame(f, captureMonitorIndex_.load(), curPkts);
      for (auto& p : curPkts) PushUrgent(std::move(p));

      // Encode when content changed, or when a full picture is still owed
      // (first frame after start / after ACCESS_LOST re-create). A static
      // desktop produces no dirty rects at all, so without the forced first
      // frame a peer would connect and never see anything.
      // A pointer-only frame has no image in the ring; an owed picture stays
      // owed and is served from the retained frame below.
      const bool force = f.copied && forceFullFrame_.exchange(false);
      if (f.copied && (f.hadDirtyRects || force)) {
        CaptureWork cw{f.contentVer, f.qpcCapture, false, force};
        if (encQueue_.Push(cw)) {
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
          ring_.Publish(bslot, ++publishSeq);
          const uint64_t cv = gens_.contentVer.fetch_add(1) + 1;
          lastContentChangeQpc_ = now;
          CaptureWork cw{cv, now, false, true};
          if (encQueue_.Push(cw)) {
            oweFirstPicture = false;
            forceFullFrame_ = false;
            WVD_LOGI("queued a GDI bootstrap frame (idle desktop; DDA reports only "
                     "changes) — peer gets picture #1 (§4.1)");
          }
        }
      }
      if (!oweFirstPicture && forceFullFrame_.load() && ring_.RepublishNewest(++publishSeq)) {
        // A keyframe is owed (recovery request) but the desktop is still:
        // re-encode the retained frame so the IDR actually goes out.
        forceFullFrame_ = false;
        CaptureWork cw{gens_.contentVer.load(), now, false, true};
        encQueue_.Push(cw);
      } else if (lastContentChangeQpc_ && gens_.contentVer.load() != refinedContentVer_ &&
                 now - lastContentChangeQpc_ > MsToQpc(cfg_.quietMsRefine) &&
                 ring_.RepublishNewest(++publishSeq)) {
        // Refine the newest picture already sitting in the ring (§6.4). It has
        // to be re-offered: the encoder already consumed it once, and
        // AcquireLatest used to report "nothing new", so refinement never ran.
        const uint64_t cv = gens_.contentVer.load();
        CaptureWork cw{cv, now, true, false};
        if (encQueue_.Push(cw)) refinedContentVer_ = cv;
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
        // Re-read every output: the new size moves the monitor's desktop
        // rectangle (and its neighbours'), which input mapping depends on.
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
  const int64_t frameInterval = MsToQpc(1000.0 / encDesc_.fpsNum);
  int64_t nextDue = QpcNow();
  uint64_t n = 0;
  PrecisionSleeper sleeper;

  while (!stopReq_) {
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
    ring_.Publish(slot, ++n);

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

    std::vector<std::vector<uint8_t>> cur;
    cursor_->OnCapturedFrame(cf, 0, cur);
    for (auto& p : cur) PushUrgent(std::move(p));

    CaptureWork cw{cf.contentVer, cf.qpcCapture, false, false};
    encQueue_.Push(cw);
    {
      std::lock_guard<std::mutex> lk(statsMutex_);
      ++capFrames_;
      if (lastCaptureQpc_) captureGapMaxMs_ = (std::max)(captureGapMaxMs_, QpcDeltaMs(lastCaptureQpc_, cf.qpcCapture));
      lastCaptureQpc_ = cf.qpcCapture;
    }

    nextDue += frameInterval;
    const int64_t now = QpcNow();
    if (nextDue > now) {
      sleeper.SleepUntilQpc(nextDue);
    } else {
      nextDue = now;  // fell behind: drop cadence, never queue up (§6.6)
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Encode thread
// ---------------------------------------------------------------------------

bool MediaSession::ApplyBitrate(uint32_t kbps, const char* why) {
  std::lock_guard<std::mutex> lk(encMutex_);
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

  // Account for both application and MsQuic queues before encoding more.
  // Stale captures are superseded by the newest contents of the capture ring.
  const int64_t frameQpc = MsToQpc(1000.0 / double(encDesc_.fpsNum));
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
    const uint32_t peerLimit = channel_->VideoBitrateLimitKbps();
    const uint32_t kbps = peerLimit ? (std::min)(peerLimit, linkRate.target()) : linkRate.target();
    if (kbps == appliedKbps) return;
    canSetBitrate = ApplyBitrate(kbps, peerLimit && kbps == peerLimit ?
                                      "headset bitrate limit" : "link queue pressure");
    if (canSetBitrate) appliedKbps = kbps;
  };

  while (!stopReq_) {
    auto work = encQueue_.Pop(std::chrono::milliseconds(100));
    if (!work) {
      // A still desktop sends nothing: the target can still climb back.
      if (adapt) linkRate.OnIdle(QpcDeltaMs(rateClock, QpcNow()));
      updateRate();
      continue;
    }

    if (channel_) {
      const int64_t waitStart = QpcNow();
      const size_t transportBudget = (std::max)(size_t(64 * 1024),
          size_t(appliedKbps) * 250 / (std::max)(1u, cfg_.fps));
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
          work = newer;
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

    // Hold the ring's lifetime guard while using a slot: a mode change on the
    // capture thread re-allocates the slots and must not pull them out from
    // under us.
    FrameRing::SharedGuard guard(ring_);

    UINT slot;
    uint64_t ringSeq;
    if (!ring_.AcquireLatest(slot, ringSeq)) continue;

    if (!converterReady_) continue;

    EncodedAu au;
    bool encSwitchFailed = false;
    uint64_t codecGen = 0;
    double convMs = 0;
    HRESULT hr = S_OK;
    {
      // Convert and encode under one lock: a mode change swaps the encoder
      // (and its picture size) on the capture thread.
      std::lock_guard<std::mutex> lk(encMutex_);
      if (!encoder_) break;

      // BGRA ring slot → NV12 texture at the encode size, on the GPU. Only an
      // encoder without texture input asks for the CPU copy.
      const int64_t convT0 = QpcNow();
      ID3D11Texture2D* nv12Tex = nullptr;
      hr = converter_.ConvertToTexture(ring_.SlotSrv(slot), ring_.width(), ring_.height(),
                                       encDesc_.width, encDesc_.height, cfg_.chroma, &nv12Tex);
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
      sp.contentVer = work->contentVer;
      sp.qpcCapture = work->qpcCapture;
      sp.qualityBoost = work->refinement;

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
          encSwitchFailed = true;
        } else {
          encoder_ = std::move(next);
          gens_.codecGen.fetch_add(1);
          codecConfigPending_ = true;
          csd_.clear();
          forcedKeyPending = true;
        }
        au.bytes.clear();
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
      codecGen = gens_.codecGen.load();
    }
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

    // Media packet (§7 header with every generation).
    PacketWriter w(PacketKind::Media, picSeq);
    MediaPayloadHeader mh{};
    mh.sessionGen = gens_.sessionGen.load();
    mh.layoutGen = gens_.layoutGen.load();
    mh.codecGen = codecGen;
    mh.picSeq = uint32_t(picSeq);
    mh.contentVer = uint32_t(au.contentVer);
    mh.deadlineQpc = work->qpcCapture + MsToQpc(cfg_.deadlineMs);
    mh.monitor = captureMonitorIndex_.load();
    mh.slice = 0;
    mh.sliceCount = 1;  // P0: one unit per picture (§6.6 tile rules later)
    mh.flags = uint16_t((au.key ? 1u : 0u) | (work->refinement ? 2u : 0u) | 4u /*lastSlice*/);
    mh.dataSize = uint32_t(au.bytes.size());
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
          WVD_LOGI("peer hello: proto %u mpps<=%u av1=%u thermal=%u audio=%u name='%s'",
                   h->protocolVersion, h->maxDecodedPixelsPerSec / 1000000, h->wantsAv1,
                   h->thermalState, (h->reserved & kHelloWantsAudio) ? 1u : 0u, name);
          SetAudioWanted((h->reserved & kHelloWantsAudio) != 0);
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
  if (r->codecGen != gens_.codecGen.load()) {
    WVD_LOGV("recovery for stale codecGen — ignored (§6.6)");
    return;
  }
  std::lock_guard<std::mutex> lk(encMutex_);
  if (!encoder_) return;
  // §6.6: try reference invalidation / LTR first (module decides), else IDR;
  // P0 modules implement IDR + intra-refresh; LTR/refval negotiation Phase 1.
  encoder_->ForceKeyframe();
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
  }
  return s;
}

}  // namespace wvd

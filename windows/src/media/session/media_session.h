// Media session — the pipeline described by §4.1 Path D + §6.6:
//
//   capture(DDA) → full-frame ring → [bounded] → NV12 convert → encode
//       → [bounded] → packetise (generations + deadline) → transport
//
// The picture stays on the GPU from the desktop surface to the encoder input
// (hardware tiers). Capture is paced to the stream frame rate, the encoded
// size is kept inside a pixel budget, and a slow link is answered before the
// encoder (skip + lower bitrate) instead of after it (drop + keyframe).
//
// Implements: generations, bounded source queues and ordered encoded output,
// deadline telemetry, recovery (IDR / LTR /
// ref-invalidation with T_rec), refinement timer T_q cancelled when
// content_ver moves on, hot tier-switch on encoder error, per-stage stats (§8).
#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "audio/loopback_capture.h"
#include "capture/dda_capture.h"
#include "depth/game_depth_capture.h"
#include "depth/synthetic_depth.h"
#include "wvd/depth.h"
#include "cursor/cursor_source.h"
#include "dx.h"
#include "encode/iencoder.h"
#include "preprocess/nv12_convert.h"
#include "wvd/bounded_queue.h"
#include "wvd/coords.h"
#include "wvd/gen.h"
#include "wvd/ipc.h"
#include "wvd/power.h"
#include "display_modes.h"
#include "screen_curtain.h"
#include "wvd/protocol.h"
#include "wvd/media_channel.h"
#include "wvd/wire1_msg.h"

namespace wvd {

struct StereoOutputSize {
  uint32_t width = 0, height = 0;
  bool halfSbs = false;  // squeeze a full SBS source before applying the size limit
};

struct SessionConfig {
  std::wstring monitorName;      // "\\\\.\DISPLAYn"; empty = primary
  uint32_t fps = 60;
  uint32_t bitrateKbps = 60000;
  // "auto" (best tier) | "hevc" | "h264" (preferred codec, best tier) |
  // "null" (pipeline test encoder) | an encoder name from `ndisplay diag caps`.
  std::string codec = "auto";
  uint32_t deadlineMs = 50;      // presentation deadline beyond capture stamp
  uint32_t quietMsRefine = 250;  // T_q: refinement after this quiet time (§6.4)
  std::wstring inputPipe;        // optional: forward input to helper
  uint64_t inputToken = 0;       // arming token from the input role (§4.7-4); 0 = cannot inject
  ChromaFilter chroma = ChromaFilter::TextAware;
  uint32_t synWidth = 1280;   // synthetic source size (§--synthetic)
  uint32_t synHeight = 720;
  // Override the default pixel budget (4K, or twice 4K for full SBS).
  // A value of 0 removes this ceiling; decoder limits still apply to stereo.
  std::optional<uint64_t> maxEncodePixels;
  msg::StereoLayout stereoLayout = msg::StereoLayout::None;
  StereoOutputSize stereoOutputSize;
  // `bitrateKbps` is the ceiling. While the link cannot keep up the session
  // lowers the encoder's target (never below the floor; 0 = ceiling / 8) and
  // raises it again once the backlog is gone.
  bool adaptiveBitrate = true;
  uint32_t minBitrateKbps = 0;
  // Longest the encoder is held back for a link that is behind before it
  // encodes anyway (a stalled peer then overflows the send queue, which
  // recovers with a keyframe). 0 = never hold back.
  uint32_t backpressureMs = 250;
  uint32_t gopSeconds = 0;    // 0 = keyframes on demand only
  // The headset may change the streamed monitor's mode (its size menu) for
  // the length of the session. Off: only the current mode is advertised.
  bool allowModeChange = true;
  // Send this PC's sound (loopback of the default playback device) while the
  // headset asks for it. Off: the headset's request is refused and logged.
  bool allowAudio = true;
  // Black out this PC's own screens while a headset is connected, like the
  // Mac's display while Mac Virtual Display runs (ScreenCurtain).
  bool screenOff = false;
  // Embedded game depth (GAME_DEPTH_HOST_PLAN.md §3–6). H1 ships the wire,
  // layout and encode path with a SYNTHETIC source: a left-to-right ramp plus
  // a nearer square following the pointer (--depth-test-pattern). A monitor
  // is packed only while the headset advertises the capability AND requests
  // depth for this monitor AND a depth source is attached (§3.3 rule 1).
  bool depthTestPattern = false;
  bool gameDepth = false;  // opted-in D3D12 games running the NDisplay ReShade add-on
  // Clear DEPTH_VALID on every Nth generated frame (0 = never): drives the
  // H1 acceptance cases for depth gaps without a hook.
  uint32_t depthTestDropEvery = 0;
};

// Encoded picture size for a source of srcW×srcH under a pixel budget: even
// dimensions, aspect ratio preserved, never larger than the source.
void FitEncodeSize(uint32_t srcW, uint32_t srcH, uint64_t maxPixels, uint32_t& outW,
                   uint32_t& outH);

struct StereoEncodePlan {
  uint32_t width, height, fps;
  msg::StereoLayout layout;
};

// Chooses coded dimensions and packing for an already packed stereo desktop.
// A full SBS source that cannot fit is squeezed horizontally to half SBS.
StereoEncodePlan PlanStereoEncode(uint32_t width, uint32_t height, uint32_t fps,
                                  msg::StereoLayout layout, uint64_t maxPixels,
                                  VideoDecodeLimits limits, bool forceHalfSbs = false,
                                  StereoOutputSize output = {});

class MediaSession {
 public:
  MediaSession(SessionConfig cfg, GenCounters& gens);
  ~MediaSession();

  // A shared reference: the QUIC transport keeps its own owner alive
// while msquic callbacks fire, so the session must not own it outright.
HRESULT Start(std::shared_ptr<MediaChannel> channel /*may be null*/);
  void Stop();
  bool running() const { return running_; }
  // True once the peer vanished / a send failed. The host tears the session
  // down and re-accepts so a headset can reconnect (§4.2: reconnect < 3s).
  bool peerGone() const { return stopReq_.load(); }
  bool synthetic = false;  // feed the ring from a CPU pattern (no DDA)

  struct Snapshot {
    uint64_t framesCaptured, framesEncoded, framesSent, framesDroppedLate;
    uint64_t missedUpdates;   // content change not displayed by deadline (§8)
    uint32_t capQueue, encQueue, sendQueue;
    double encP50Ms, encP95Ms;
    double convP50Ms, convP95Ms;  // NV12 convert submission (GPU work is async)
    uint32_t kbpsOut, fpsOut;
    std::string encoder;
    PointerState pointerState;
    uint32_t targetKbps;          // current encoder target (adaptive)
    uint32_t encWidth, encHeight; // encoded picture size
    uint64_t skippedForLink;      // pictures skipped before encode: link behind
    bool zeroCopy;                // encoder consumes GPU textures
    double captureGapMaxMs = 0;   // worst inter-picture gap in this reporting window
    double sendGapMaxMs = 0;
    double sendP95Ms = 0;         // time spent writing one encoded picture
    // Embedded depth (game-depth plan): packing state of the current codec
    // generation and the count of pictures that carried DEPTH_VALID.
    bool depthPacking = false;
    uint32_t depthMapWidth = 0, depthMapHeight = 0;
    uint64_t depthValidFrames = 0;
  };
  Snapshot GetSnapshot();

 private:
  struct SendWork {
    std::vector<uint8_t> framed;  // one Media packet
    int64_t deadlineQpc = 0;
  };
  // Identity of a PUBLISHED ring frame. AcquireLatest hands back a slot and
  // its sequence number; colour/depth pairing and every Media header field
  // must use the identity the capture thread attached to that sequence —
  // never the work item that happened to wake the encoder (capture can get
  // ahead between Pop and AcquireLatest, which would pair frame B's colour
  // with frame A's depth).
  struct FrameIdentity {
    uint64_t ringSeq = 0;
    uint64_t contentVer = 0;
    int64_t qpcCapture = 0;
    bool refinement = false;
    uint32_t videoEpoch = 1;
    uint64_t layoutGen = 0;
    std::shared_ptr<const DepthFrame> gameDepth;
  };
  // Capture thread: record before making a slot and sequence visible to the encoder.
  void RecordFrameIdentity(uint64_t ringSeq, uint64_t contentVer, int64_t qpcCapture,
                           bool refinement, uint32_t videoEpoch,
                           std::shared_ptr<const DepthFrame> gameDepth = {});
  // Encode thread: resolve the acquired frame's identity.
  bool FindFrameIdentity(uint64_t ringSeq, FrameIdentity& out);

  bool CaptureDda();       // capture thread body
  bool WaitOnStop(uint32_t ms) {
    // Interruptible sleep: true if stop was requested.
    for (uint32_t i = 0; i < ms && !stopReq_; i += 10) Sleep(ms - i < 10 ? ms - i : 10);
    return stopReq_.load();
  }
  void EncodeLoop();
  void SendLoop();
  void RecvLoop();

  // Urgent (control/cursor) and media enqueue; both wake the send thread so a
  // cursor packet never waits for a media poll interval.
  void PushUrgent(std::vector<uint8_t> pkt);
  void PushMedia(SendWork sw);

  // CodecConfig for the CURRENT encoder + parameter sets; caller holds
  // encMutex_. Sent by the encode thread right before the first picture of
  // every codec generation, so it can carry the real codec and its VPS/SPS/PPS.
  void QueueCodecConfigLocked();
  void SendMonitorLayout();
  // Rebuild the encoder for a new surface size (display mode change). Takes
  // encMutex_, bumps codecGen; CodecConfig follows with its first picture.
  bool ReconfigureEncoder(UINT width, UINT height);
  void ConfigureEncodeSizeLocked(UINT width, UINT height, bool forceHalfSbs = false);
  bool FallbackToHalfSbsLocked(UINT width, UINT height);
  StereoEncodePlan ChooseEncodePlan(UINT width, UINT height, bool forceHalfSbs = false) const;
  bool RecreateEncoderLocked();
  void PublishEncodeModeLocked();
  uint32_t VideoEpoch() const { return channel_ ? channel_->VideoEpoch() : 1; }
  uint32_t EffectiveFrameRate() const;
  // Caller holds encMutex_. False once the encoder
  // turns out not to support it.
  bool ApplyBitrate(uint32_t kbps, const char* why);
  void ForwardInputToHelper(PacketView pv);
  void HandleRecovery(PacketView pv);
  // Display size (headset menu). The Locked variants need layoutMutex_, so a
  // list is never queued ahead of the layout generation it names.
  void HandleModeChange(PacketView pv);
  void SendDisplayModes(uint32_t requestId, ModeChangeStatus status);
  void SendDisplayModesLocked(uint32_t requestId, ModeChangeStatus status);
  void RestoreModeIfChanged();
  // monitors_ / layout_ / captureMonitorIndex_ from the current outputs; the
  // captured monitor takes the capture size w x h. Caller holds layoutMutex_.
  void BuildLayoutLocked(UINT w, UINT h);
  void SendMonitorLayoutLocked();
  void RecordStageTimes(const EncodedAu& au);

  // Embedded depth (game-depth plan §3–6).
  // §3.3 rule 1: capability AND request AND an attached source. Caller holds
  // encMutex_; on true, `layout` holds the strip geometry for encColorW×encH.
  bool DepthWantedLocked(EmbeddedDepthPayload& layout);
  bool DepthRequested();
  bool DepthLayoutFits(uint32_t width, uint32_t height, EmbeddedDepthPayload& layout) const;
  // Encode thread (encMutex_ held): a packing or layout change is a codec
  // generation change — new coded size, CodecConfig with/without the layout,
  // IDR. Frame-to-frame gaps stay DEPTH_VALID, never a new generation.
  void UpdateDepthPackingLocked();
  uint32_t DepthBitrateLocked() const;  // +10% while packing (§4)
  // Capture thread: feed the test depth source for a captured frame. Pointer
  // coords are colour-picture pixels.
  void PostDepthFrame(int64_t qpc, int32_t ptrX, int32_t ptrY);

  // Input helper connection (§4.4/§4.7). Connects, arms with the operator's
  // token and learns the helper's input generation. Re-tried at most every 2 s.
  bool EnsureInputArmed();
  bool InputRequest(uint32_t cmd, const void* extra, size_t extraLen, IpcStatusResponse* st);
  void ReleaseAndDisarmInput();

  SessionConfig cfg_;
  GenCounters& gens_;

  // DX / capture
  ComPtr<IDXGIFactory1> factory_;
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> ctx_;
  ComPtr<IDXGIOutput> output_;
  DxOutputInfo outputInfo_;
  FrameRing ring_;
  std::unique_ptr<DdaCapture> dda_;
  std::unique_ptr<CursorSource> cursor_;
  Nv12Converter converter_;
  bool converterReady_ = false;

  // Encode
  EncoderInitDesc encDesc_;
  std::unique_ptr<IVideoEncoder> encoder_;
  std::string encoderName_;
  std::mutex encMutex_;
  msg::StereoLayout encodedStereoLayout_ = msg::StereoLayout::None;
  UINT encSourceW_ = 0, encSourceH_ = 0;  // source dimensions matching the encoder
  // Committed sizes for the display menu. Reporting reads this without
  // taking encMutex_ or waiting for conversion/encoding.
  std::mutex encodeModeMutex_;
  struct EncodeMode { UINT sourceW, sourceH, colorW, colorH; };
  EncodeMode encodeMode_{};
  std::atomic<uint32_t> encodeRateLimit_{0};  // stereo pacing cap shared with capture
  bool codecConfigPending_ = true;   // guarded by encMutex_
  std::vector<uint8_t> csd_;         // parameter sets of the current codecGen

  // --- Embedded game depth (game-depth plan; H1 = synthetic source) --------
  std::atomic<bool> clientDepthCap_{false};  // DecoderCaps.embedded_depth (via Hello)
  std::atomic<bool> depthEval_{true};        // re-evaluate packing on the encode thread
  std::mutex depthReqMutex_;
  std::vector<uint16_t> depthMonitorsReq_;   // StreamSettings.depth_monitors (0-based)
  // Packing state of the CURRENT codec generation (encMutex_):
  bool depthPacking_ = false;
  EmbeddedDepthPayload depthLayout_{};
  uint32_t encColorW_ = 0, encColorH_ = 0;   // colour encode size, without the strip
  uint64_t depthValidFrames_ = 0;            // pictures sent with DEPTH_VALID
  std::unique_ptr<DepthSource> depthSrc_;   // synthetic test pattern
  std::unique_ptr<GameDepthCapture> gameDepth_;  // capture thread only
  std::atomic<bool> gameDepthAttached_{false};
  // Published-frame identity history (see FrameIdentity): written by the
  // capture thread, read by the encode thread.
  std::mutex identMutex_;
  std::deque<FrameIdentity> idents_;
  DepthNormalizer depthNorm_;                // encode thread only
  int64_t lastDepthNormQpc_ = 0;             // encode thread only
  int32_t depthPtrX_ = 0, depthPtrY_ = 0;    // capture thread: last known pointer
  // Re-entrant claim on the encoder/description state: the capture thread
  // only ever TRYs to take it (reading the strip layout must never stall a
  // capture behind an in-flight convert+encode).
  CRITICAL_SECTION encCs_;

  // Queues (§6.6: bounded, newest-wins, reported)
  // The queue wakes the encoder for one video epoch. The ring owns the frame identity.
  BoundedQueue<uint32_t> encQueue_{2, OverflowPolicy::NewestWins};
  BoundedQueue<SendWork> sendQueue_{4, OverflowPolicy::Reject};
  // Ahead of media: control (layout, codec config, cursor shapes) must all
  // arrive, so it never drops silently; cursor positions are newest-wins.
  BoundedQueue<std::vector<uint8_t>> controlQueue_{64, OverflowPolicy::Reject};
  BoundedQueue<std::vector<uint8_t>> cursorQueue_{4, OverflowPolicy::NewestWins};
  // Audio after cursor, before media: ~100 packets/s; a stalled peer loses the
  // oldest audio rather than delaying control traffic.
  BoundedQueue<std::vector<uint8_t>> audioQueue_{16, OverflowPolicy::NewestWins};
  // Audio follows the headset's latest Hello (it re-sends one when sound is
  // switched on or off, or all its windows hide or show). Recv thread, then Stop().
  std::unique_ptr<LoopbackCapture> audio_;
  bool audioOn_ = false;
  bool audioRefusedLogged_ = false;
  std::atomic<uint64_t> audioSeq_{0};
  void SetAudioWanted(bool wanted);
  HANDLE sendWake_ = nullptr;

  std::shared_ptr<MediaChannel> channel_;
  KeepAwake keepAwake_;  // display + system stay on while a peer is connected
  ScreenCurtain curtain_;  // --screen-off: this PC's screens black while connected

  // Input helper state (recv thread only, plus Stop()).
  PipeClient inputPipe_;
  bool inputArmed_ = false;
  bool inputRevoked_ = false;      // helper disarmed us: no auto re-arm this session
  bool inputTokenRejected_ = false;
  uint64_t helperInputGen_ = 0;
  uint64_t helperInstance_ = 0;
  ULONGLONG lastInputConnectTry_ = 0;

  bool CaptureSynthetic();  // CPU test pattern (no display) — --synthetic
  std::thread thCapture_, thEncode_, thSend_, thRecv_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopReq_{false};

  // Refinement state (§6.4)
  int64_t lastContentChangeQpc_ = 0;
  uint64_t refinedContentVer_ = 0;

  // Set after start and after every capture re-create: the NEXT captured frame
  // must be encoded and sent even if the desktop is perfectly static, so a
  // newly connected peer always gets a real picture. Without this, a still
  // desktop yields dirty-rect-free frames and nothing is ever sent.
  std::atomic<bool> forceFullFrame_{true};
  // Recovery coalescing (§6.6). A Wi-Fi loss burst makes the headset repeat
  // its keyframe request every T_rec (100 ms); forcing an IDR for each one
  // floods the weakened link with the largest pictures there are (measured:
  // 15-23 IDRs per 2 s burst). The newest keyframe emitted, and a forced one
  // still on its way out of the encoder, answer a request already (WIRE.md §8).
  std::atomic<uint32_t> lastKeyPicSeq_{0};
  std::atomic<int64_t> lastKeyQpc_{0};
  std::atomic<int64_t> keyOwedSinceQpc_{0};  // 0 = no forced keyframe pending

  // Windows streams one selected output. Index 0 on the capture side is
  // always monitor ID 1 on WIRE v1, irrespective of DXGI output ordering.
  static constexpr uint16_t kCaptureMonitorIndex = 0;

  // Display mode of the captured monitor (\\.\DISPLAYn; empty = synthetic).
  // A change made for the headset is undone when the session ends.
  std::wstring captureDevice_;
  bool modeChanged_ = false;          // recv thread, then Stop()
  DisplayModeInfo originalMode_;

  // Stats
  std::mutex statsMutex_;
  std::vector<double> encTimesMs_;
  std::vector<double> convTimesMs_;
  uint64_t capFrames_ = 0, encFrames_ = 0, sentFrames_ = 0, lateDrops_ = 0;
  uint64_t missedUpdates_ = 0;
  uint64_t skippedForLink_ = 0;
  uint64_t bytesSent_ = 0;
  LARGE_INTEGER lastStatsTick_{};
  uint64_t lastSentFrames_ = 0, lastBytes_ = 0;
  int64_t lastCaptureQpc_ = 0, lastSendQpc_ = 0;
  double captureGapMaxMs_ = 0, sendGapMaxMs_ = 0;
  std::vector<double> sendTimesMs_;

  // Layout: rebuilt by the capture thread on a size change, read by the
  // recv thread for input mapping.
  std::mutex layoutMutex_;
  LayoutMapper layout_;
  std::vector<MonitorDesc> monitors_;
};

}  // namespace wvd

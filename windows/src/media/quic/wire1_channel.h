// WIRE.md v1 host session (proto/WIRE.md) — the UDP side of the media worker.
//
// A Wire1Channel is one authenticated headset, presented to the existing media
// pipeline as an MediaChannel: MediaSession keeps producing its packed
// "backend A" packets and this adapter turns them into what WIRE v1 wants —
//
//   MonitorLayout / CodecConfig / CursorShape / DisplayModes / HelloAck /
//   SessionState / Pong / HostStats   -> length-prefixed protobuf envelopes on
//                                        the reliable control stream
//   Media                              -> length-prefixed NALs, FEC, QUIC datagrams
//   CursorPos                          -> datagram (urgent, unpaced)
//   Audio                              -> datagrams (~5 ms each)
//
// and back: the headset's Ping, ClientStats, RecoveryRequest, Visibility,
// StreamSettings, DisplayModeRequest, InputEvents and InputRelease become the
// small set of packets the pipeline already understands, so capture, encode,
// generations, recovery and the privileged input helper stay in the pipeline.
//
// The adapter owns everything the packed protocol has no notion of:
// pairing and pinned-identity authentication (§7), the datagram flow binding
// (FlowHello <-> SessionState.media_flow_bound), Reed-Solomon FEC (§5), the
// pacer, NACK retransmission from a short picture history, and the loss-driven
// FEC ratio.
//
// Lifetime: the listener creates a channel when a QUIC connection completes
// TLS, and hands it out from Accept() only after authentication. MediaSession
// holds a shared_ptr; the listener holds weak_ptrs for its callbacks, so a
// connection that dies mid-session can never pull the object out from under
// the pipeline threads.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "quic_host.h"
#include "wdauth.h"
#include "wvd/fec.h"
#include "wvd/gen.h"
#include "wvd/protocol.h"
#include "wvd/media_channel.h"
#include "wvd/wire1_msg.h"

namespace wvd {

class HostIdentity;
enum class WorkstationState { Available, Locked, SecureDesktop };

struct Wire1Config {
  // HelloAck identity. hostId is stable across reconnects; the client keys its
  // pin on it, so derive it from something that does not change.
  std::string hostId;
  std::string hostName;
  std::string hostOs;
  // Must match SessionConfig::deadlineMs: the packed Media packet carries only
  // the absolute deadline, and capture_time = deadline - this.
  uint32_t deadlineMs = 50;
  // WIRE.md §9 presentation budget carried in every shard header.
  uint32_t deadlineBudgetMs = 100;
  // Pairing. When false, an unknown device gets AUTH_PAIRING_REQUIRED.
  bool pairingAllowed = true;
  // Trusted lab network: accept pairing without the operator confirming the
  // SAS. Logged loudly, because it removes the man-in-the-middle check.
  bool autoAcceptPairing = false;
  // The operator's input consent (§4.7). Reports AUTH ARMED so the headset
  // sends input; the media worker still needs a helper token to inject.
  bool inputAllowed = false;
  // Pairing requests older than this are refused (WIRE.md §7: 120 s).
  uint32_t pairingTimeoutMs = 120000;
};

// What the operator (the control panel) has to see and answer.
class IWire1Observer {
 public:
  virtual ~IWire1Observer() = default;
  // Ask the human to compare the six digits with the headset, then answer with
  // Wire1Listener::DecidePairing(requestId, accept).
  virtual void OnPairingRequest(uint64_t requestId, const std::string& deviceName,
                                const std::string& sas) = 0;
  virtual void OnPairingDecided(uint64_t requestId, bool accepted) = 0;
};

// Datagram pacer (port of mac/WVDHostKit/Pacer.swift): msquic drops datagrams
// it cannot send immediately, so an unpaced 4K keyframe loses most of itself.
// Each picture is spread over part of a frame interval at a token-bucket rate.
class DatagramPacer {
 public:
  struct Config {
    double maxRateBps = 600e6;
    double minRateBps = 60e6;
    double spreadFraction = 0.5;
    uint32_t quantumUs = 250;
    static constexpr size_t maxQueuedBytes = 2u << 20;
  };
  // Sends one datagram; false when the peer is not reading (a loss).
  using Sender = std::function<bool(std::vector<uint8_t>&&, uint32_t videoEpoch)>;

  DatagramPacer() = default;
  ~DatagramPacer() { Stop(); }
  DatagramPacer(const DatagramPacer&) = delete;
  DatagramPacer& operator=(const DatagramPacer&) = delete;

  void Start(Sender sender);
  void Stop();
  void Clear();
  // One picture's datagrams. Returns false when the queue is full: bounded by
  // design, the receiver notices the gap and asks for recovery.
  bool Enqueue(std::vector<std::vector<uint8_t>>&& datagrams, double frameIntervalUs,
               double avgBitrateBps, int64_t deadlineQpc, uint32_t videoEpoch);
  size_t BacklogBytes() const;
  uint64_t DroppedBytes() const { return droppedBytes_.load(); }

 private:
  void Run();

  Config cfg_;
  Sender sender_;
  struct Item {
    std::vector<std::vector<uint8_t>> datagrams;
    size_t next = 0;
    size_t remainingBytes = 0;
    double rateBps = 0;
    int64_t deadlineQpc = 0;
    uint32_t videoEpoch = 0;
  };
  mutable std::mutex m_;
  std::condition_variable available_;
  std::deque<Item> queue_;
  size_t queuedBytes_ = 0;
  bool running_ = false;
  std::thread th_;
  std::atomic<uint64_t> droppedBytes_{0};
};

// One headset connection, post-TLS. Not thread safe by itself: every entry
// point (msquic callbacks, the pipeline's Send, Close) takes its own lock.
class Wire1Channel : public MediaChannel,
                     public std::enable_shared_from_this<Wire1Channel> {
 public:
  struct Context {
    GenCounters* gens = nullptr;
    Wire1Config cfg;
    std::vector<uint8_t> certSha256;  // ours: what the client pins
    IWire1Observer* observer = nullptr;
    // This session's generation: every datagram, FlowHello and input stream is
    // stamped with it, so an older session's packets can never be accepted.
    uint32_t sessionGen = 1;
    // Host-side pairing store operations, kept out of the channel so the
    // store's file locking lives in one place.
    std::function<std::optional<PairedDevice>(const std::string&)> findDevice;
    std::function<void(const PairedDevice&)> saveDevice;
    // One streaming session per host (WIRE.md §1): claim it, false when
    // another headset is already streaming.
    std::function<bool()> claimActive;
  };

  explicit Wire1Channel(Context ctx);
  ~Wire1Channel() override;

  // ---- MediaChannel, used by MediaSession -----------------------------
  bool Send(const uint8_t* data, size_t len) override;
  std::optional<std::vector<uint8_t>> RecvPacket(uint32_t timeoutMs) override;
  void Close() override;
  bool IsConnected() const override;
  size_t BacklogBytes() const override;
  uint32_t VideoBitrateLimitKbps() const override { return clientBitrateKbps_; }
  uint32_t VideoFrameRateLimit() const override { return clientFrameRate_; }
  VideoDecodeLimits GetVideoDecodeLimits() const override {
    return {peerMaxWidth_.load(), peerMaxHeight_.load(), peerMaxPixelRate_.load()};
  }
  bool PreferVideoQuality() const override { return clientPreferQuality_; }
  uint32_t VideoEpoch() const override { return videoEpoch_; }
  void SetWorkstationState(WorkstationState state);

  // ---- driven from the listener's msquic callbacks ------------------------
  void OnConnected(QuicConn conn, const std::vector<uint8_t>& peerCertSha256);
  void OnStream(QuicStream stream);
  void OnStreamData(QuicStream stream, const uint8_t* data, size_t len);
  void OnStreamClosed(QuicStream stream);
  bool OnDatagram(const uint8_t* data, size_t len);  // true on initial flow binding
  // The QUIC connection is gone; the pipeline learns through IsConnected().
  void OnClosed(uint32_t status);

  // ---- state for the worker / panel ---------------------------------------
  // True once authenticated: only then may the media pipeline start.
  const std::string& peerDeviceId() const { return peerDeviceId_; }
  const std::string& peerName() const { return peerName_; }
  const std::string& peerAddress() const { return peerAddress_; }
  // Operator answer to a pairing prompt (forwarded from the panel).
  void DecidePairing(uint64_t requestId, bool accept);
  // Live link numbers for the worker's log line.
  struct Link {
    uint64_t datagramsSent = 0, retransmitted = 0, datagramDrops = 0, pacerDrops = 0;
    double lossPercent = 0, rttMs = 0;
    double fecPercent = 10;
  };
  Link link() const;

  // The six digits the operator compares against the headset (§7.2).
  const std::string& pairingSas() const { return pairingSas_; }
  uint64_t pairingRequestId() const { return pairingRequestId_; }
  bool inputAllowed() const {
    std::lock_guard<std::mutex> lk(m_);
    return ctx_.cfg.inputAllowed;
  }
  // Arming or disarming input is the operator's call, and it takes effect
  // immediately: SessionState{input: armed|disarmed} tells the headset (§7.4),
  // and disarming releases every key and button the headset is holding.
  void SetInputAllowed(bool allowed);
  // Claims the single streaming slot; set by the listener before callbacks.
  void SetClaimActive(std::function<bool()> fn) { ctx_.claimActive = std::move(fn); }

 private:
  struct StreamState {
    framing::Deframer deframer{true};  // the peer writes the "WVD1" preamble
    bool preambleSent = false;
    bool isInput = false;
    bool decided = false;  // its first envelope told us which stream this is
  };

  // handshake
  void RouteEnvelope(QuicStream stream, const msg::Envelope& env);
  void HandleControl(const msg::Envelope& env);
  void HandleInput(const msg::Envelope& env);
  void HandleHello(const msg::Hello& hello);
  void HandlePairingConfirm(const msg::PairingConfirm& confirm);
  void MaybeCompletePairing();
  void FinishPairing(bool success, const std::string& reason);
  void Authenticated();
  void SendSessionState();
  void UpdateVideoVisibility();
  void SendGoodbyeAndClose(const std::string& reason);

  // media out
  void SendEnvelope(const msg::Envelope& env);  // control stream, with m_ held
  void SendEnvelopeOn(QuicStream stream, const msg::Envelope& env, StreamState& st);
  void OnMedia(const MediaPayloadHeader& mh, const uint8_t* au, size_t auBytes,
               std::unique_lock<std::mutex>& lock);
  uint32_t UsableDatagramPayload() const;
  void SendUrgent(std::vector<uint8_t> datagram);
  void OnCursorPos(const CursorPosPayload& cp);
  void OnCursorShape(const CursorShapePayload& shape, const uint8_t* pixels, uint32_t pixelBytes);
  void OnAudioPacket(const AudioPayload& a, const int16_t* pcm, uint32_t frames);
  void OnLegacyLayout(const uint8_t* payload, uint32_t size);
  void OnLegacyCodecConfig(const uint8_t* payload, uint32_t size);
  void OnLegacyDisplayModes(const uint8_t* payload, uint32_t size);
  void Retransmit(const dg::Nack& nack);
  void StatsLoop();

  // WIRE in -> packed packets for the pipeline
  void QueueForPipeline(std::vector<uint8_t> framedPacket);
  void QueueHello(bool wantsAudio, const std::string& deviceName);
  // StreamSettings.depth_monitors: headset 1-based ids -> 0-based layout indices.
  void QueueDepthMonitors(const std::vector<uint32_t>& monitorIds);
  void QueueRecovery(uint32_t codecGen, uint32_t kind, uint32_t refPicSeq);
  void QueueModeChange(const msg::DisplayModeRequest& r);
  void QueueInput(const msg::InputEvents& batch);
  void QueueInputRelease(uint32_t reason);
  uint32_t sessionGen() const { return ctx_.sessionGen; }
  void SendPong(const msg::Ping& ping);
  void UpdateFecFromStats(const msg::ClientStats& stats);
  uint64_t HostMicroseconds() const;

  mutable std::mutex m_;
  std::condition_variable statsCv_;
  // The inbound queue has its own lock: a control handler runs under m_ and
  // pushes packets for the pipeline, and std::mutex is not recursive (locking
  // it twice aborts the process).
  std::mutex inboundM_;
  std::condition_variable inboundCv_;
  Context ctx_;

  enum class Auth { AwaitingHello, Pairing, Ready, Closed } auth_ = Auth::AwaitingHello;
  QuicConn conn_ = nullptr;
  std::vector<uint8_t> peerCertSha256_;
  std::vector<uint8_t> sasExporter_, authExporter_, pairExporter_;

  QuicStream control_ = nullptr;
  QuicStream inputStream_ = nullptr;
  std::map<QuicStream, StreamState> streams_;

  // Datagram flow (WIRE.md §3): bound by the client's FlowHello.
  bool mediaFlowBound_ = false;
  uint32_t peerMaxDatagram_ = 0;
  fec::PacketizerConfig packetCfg_;
  double frameIntervalUs_ = 1000000.0 / 60.0;
  double targetBitrateBps_ = 0;

  // Picture history for NACK retransmission (WIRE.md §4.4).
  std::map<uint32_t, fec::PacketizedFrame> history_;
  size_t historyBytes_ = 0;

  // Auth state that only exists during a handshake.
  msg::Hello peerHello_;
  std::string peerDeviceId_, peerName_, peerModel_, peerAddress_;
  msg::ThermalState thermal_ = msg::ThermalState::Nominal;
  uint64_t pairingRequestId_ = 0;
  uint64_t pairingDeadlineUs_ = 0;
  std::string pairingSas_;
  bool clientConfirmedPairing_ = false;
  bool hostConfirmedPairing_ = false;
  bool pairingOutcomeSent_ = false;

  // Pipeline ingress: packed packets synthesized from WIRE messages.
  std::deque<std::vector<uint8_t>> inbound_;  // guarded by inboundM_, not m_

  // Connectivity and the datagram-drop count are read from places that must
  // never take m_ (the pipeline's wait predicate, the pacer's send callback),
  // so they are atomic instead of locked.
  std::atomic<bool> peerConnected_{true};
  std::atomic<bool> linked_{false};
  std::atomic<uint32_t> clientBitrateKbps_{0};
  std::atomic<uint32_t> clientFrameRate_{0};
  std::atomic<bool> clientPreferQuality_{true};
  // DecoderCaps.embedded_depth / max_pixel_rate (game-depth plan §3.1). Set
  // once at Hello; read without m_ from the pipeline.
  std::atomic<bool> peerEmbeddedDepth_{false};
  std::atomic<uint32_t> peerMaxWidth_{0}, peerMaxHeight_{0};
  std::atomic<uint64_t> peerMaxPixelRate_{0};
  // The HelloPayload.reserved combination last queued to the pipeline, and
  // the depth monitor list last seen from the headset (both under m_).
  uint32_t helloReservedQueued_ = 0xFFFFFFFF;
  std::vector<uint32_t> clientDepthMonitors_;
  std::atomic<uint32_t> videoEpoch_{1};
  WorkstationState workstationState_ = WorkstationState::Available;
  std::set<uint32_t> visibilityExceptions_;
  bool allMonitorsHidden_ = false;
  bool awaitingIdr_ = false;

  // Which monitor the media belongs to is only stated on the packed media
  // packet, so a codec configuration or cursor shape that arrives before the
  // first picture waits here.
  bool captureKnown_ = false;
  std::optional<msg::Envelope> pendingCodecConfig_;
  std::vector<msg::Envelope> pendingCursorShapes_;

  // Cursor / stats bookkeeping.
  uint32_t cursorSeq_ = 0;
  uint32_t shapeVerSent_ = 0;   // a position without a shape yet is undrawable
  uint32_t layoutGenOut_ = 0;
  Codec codecOut_ = Codec::HEVC;
  uint32_t codecGenOut_ = 0;
  uint32_t bitrateKbpsOut_ = 0;
  uint32_t inputGen_ = 1;
  uint64_t inputSeq_ = 0;
  bool audioWanted_ = false;
  uint16_t captureMonitorId_ = 1;
  uint64_t picturesOut_ = 0, bytesOut_ = 0;
  DatagramPacer pacer_;
  std::thread statsThread_;
  uint64_t retransmitted_ = 0;
  uint64_t lastStatsBytes_ = 0, lastStatsPictures_ = 0;
  uint64_t statWindowStartUs_ = 0;
  uint32_t idrCount_ = 0;
  double lossPercent_ = 0, rttMs_ = 0;
  // Audio packet splitting: WIRE wants <=5 ms datagrams that fit the path.
  uint32_t audioSeq_ = 0;
};

// Listens on UDP and turns authenticated connections into Wire1Channels.
class Wire1Listener : public IQuicSink {
 public:
  // `devices` is the pairing store; a channel reads it when a Hello arrives
  // and extends it when a pairing completes. The worker owns it for the
  // lifetime of the process.
  Wire1Listener(GenCounters& gens, Wire1Config cfg, HostIdentity& identity,
                PairedDeviceStore& devices);
  ~Wire1Listener() override;

  bool Listen(uint16_t port, std::string* error, bool loopbackOnly = false);
  void Stop();
  uint16_t port() const { return server_.port(); }

  // The next headset that completed authentication (nullptr on timeout or
  // Stop()). The caller holds it for the length of the session.
  std::shared_ptr<Wire1Channel> AcceptChannel(uint32_t timeoutMs);
  // The session Accept() handed out that is still connected (nullptr between
  // sessions): for link reporting and for arming input.
  std::shared_ptr<Wire1Channel> active() const;
  void SetObserver(IWire1Observer* observer) { observer_ = observer; }

  void DecidePairing(uint64_t requestId, bool accept);
  void SetPairingAllowed(bool allowed) { cfg_.pairingAllowed = allowed; }
  void SetInputAllowed(bool allowed) { cfg_.inputAllowed = allowed; }
  const Wire1Config& config() const { return cfg_; }

  // IQuicSink
  void OnQuicConnected(QuicConn conn, const std::vector<uint8_t>& peerCertSha256) override;
  void OnQuicStream(QuicConn conn, QuicStream stream) override;
  void OnQuicStreamData(QuicConn conn, QuicStream stream, const uint8_t* data,
                        size_t len) override;
  void OnQuicStreamClosed(QuicConn conn, QuicStream stream) override;
  void OnQuicDatagram(QuicConn conn, const uint8_t* data, size_t len) override;
  void OnQuicClosed(QuicConn conn, uint32_t status) override;

 private:
  std::shared_ptr<Wire1Channel> Lookup(QuicConn conn);

  GenCounters& gens_;
  Wire1Config cfg_;
  HostIdentity& identity_;
  IWire1Observer* observer_ = nullptr;
  QuicServer server_;

  mutable std::mutex m_;
  std::condition_variable cv_;
  std::map<QuicConn, std::shared_ptr<Wire1Channel>> channels_;
  std::deque<std::shared_ptr<Wire1Channel>> ready_;
  // The session currently streaming: WIRE allows one, further clients are
  // told AUTH_BUSY (WIRE.md §1).
  std::weak_ptr<Wire1Channel> active_;
  PairedDeviceStore& devices_;
  std::mutex devicesMutex_;
  bool stopping_ = false;
};

// Host id derived from the certificate: stable, and the same one a paired
// client stored (it never changes unless the identity is regenerated).
std::string Wire1HostId(const std::vector<uint8_t>& certSha256);

}  // namespace wvd

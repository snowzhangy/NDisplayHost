#include "wire1_channel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "identity.h"
#include "wdauth.h"
#include "wvd/logging.h"
#include "wvd/pacing.h"
#include "wvd/time.h"

namespace wvd {
namespace {

// proto/WIRE.md §1 protocol version this host speaks.
constexpr uint32_t kWire1ProtocolVersion = 1;

constexpr uint32_t kMinMediaDatagram = dg::MediaShardHeader::kSize + 256;
constexpr size_t kHistoryMaxBytes = 4u << 20;
std::atomic<uint64_t> nextPairingRequestId{1};

uint64_t QpcMicroseconds(int64_t qpc) {
  const auto frequency = static_cast<int64_t>(QpcFreq());
  return uint64_t(qpc / frequency) * 1000000 + uint64_t(qpc % frequency) * 1000000 / frequency;
}

template <class T>
T Clamp(T v, T lo, T hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// Length of the Annex-B start code at `p` (0 when there is none).
size_t StartCodeLength(const uint8_t* csd, size_t size, size_t p) {
  if (p + 3 <= size && csd[p] == 0 && csd[p + 1] == 0 && csd[p + 2] == 1) return 3;
  if (p + 4 <= size && csd[p] == 0 && csd[p + 1] == 0 && csd[p + 2] == 0 && csd[p + 3] == 1)
    return 4;
  return 0;
}

// CodecConfig.parameter_sets: the individual NAL units, without start codes
// (proto/wvd.proto), in the order the encoder emitted them.
std::vector<uint8_t> NextParameterSet(const uint8_t* csd, size_t size, size_t& at) {
  while (at < size) {
    const size_t sc = StartCodeLength(csd, size, at);
    if (!sc) {
      ++at;
      continue;
    }
    const size_t from = at + sc;
    size_t to = size;
    for (size_t j = from; j + 2 < size; ++j) {
      if (StartCodeLength(csd, size, j)) {
        to = j;
        break;
      }
    }
    at = to;
    if (to > from) return std::vector<uint8_t>(csd + from, csd + to);
  }
  return {};
}

}  // namespace

// =============================================================================
// DatagramPacer
// =============================================================================

void DatagramPacer::Start(Sender sender) {
  std::lock_guard<std::mutex> lock(m_);
  sender_ = std::move(sender);
  running_ = true;
  th_ = std::thread([this] { Run(); });
}

void DatagramPacer::Stop() {
  {
    std::lock_guard<std::mutex> lock(m_);
    running_ = false;
  }
  available_.notify_one();
  if (th_.joinable()) th_.join();
  std::lock_guard<std::mutex> lock(m_);
  queue_.clear();
  queuedBytes_ = 0;
}

bool DatagramPacer::Enqueue(std::vector<std::vector<uint8_t>>&& datagrams,
                            double frameIntervalUs, double avgBitrateBps, int64_t deadlineQpc) {
  if (datagrams.empty()) return true;
  size_t bytes = 0;
  for (const auto& d : datagrams) bytes += d.size();
  const double spreadSeconds = (std::max)(frameIntervalUs * cfg_.spreadFraction, 1000.0) / 1e6;
  const double rate = (std::min)(cfg_.maxRateBps,
      std::max({cfg_.minRateBps, double(bytes) * 8.0 / spreadSeconds, avgBitrateBps * 2.0}));
  {
    std::lock_guard<std::mutex> lock(m_);
    if (!running_ || bytes > cfg_.maxQueuedBytes - queuedBytes_ || QpcNow() >= deadlineQpc) {
      droppedBytes_ += bytes;
      return false;
    }
    queue_.push_back(Item{std::move(datagrams), 0, bytes, rate, deadlineQpc});
    queuedBytes_ += bytes;
  }
  available_.notify_one();
  return true;
}

size_t DatagramPacer::BacklogBytes() const {
  std::lock_guard<std::mutex> lock(m_);
  return queuedBytes_;
}

void DatagramPacer::Run() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  PrecisionSleeper sleeper;
  std::vector<std::vector<uint8_t>> batch;
  batch.reserve(64);
  double budget = 0;
  int64_t last = QpcNow();
  while (true) {
    {
      std::unique_lock<std::mutex> lock(m_);
      if (queue_.empty()) {
        available_.wait(lock, [this] { return !queue_.empty() || !running_; });
        budget = 1400;
        last = QpcNow();
      }
      if (!running_) return;

      const int64_t now = QpcNow();
      const double bytesPerMs = queue_.front().rateBps / 8000.0;
      budget = (std::min)(budget + QpcDeltaMs(last, now) * bytesPerMs,
                          1400.0 + bytesPerMs * cfg_.quantumUs / 500.0);
      last = now;
      while (!queue_.empty()) {
        auto& item = queue_.front();
        if (now >= item.deadlineQpc) {
          droppedBytes_ += item.remainingBytes;
          queuedBytes_ -= item.remainingBytes;
          queue_.pop_front();
          continue;
        }
        auto& datagram = item.datagrams[item.next];
        const size_t bytes = datagram.size();
        if (budget < double(bytes)) break;
        budget -= double(bytes);
        item.remainingBytes -= bytes;
        queuedBytes_ -= bytes;
        batch.push_back(std::move(datagram));
        if (++item.next == item.datagrams.size()) {
          queue_.pop_front();
          // Each picture has its own rate. Do not spend its unused burst
          // allowance on a smaller picture with a lower pacing rate.
          budget = (std::min)(budget, 1400.0);
          break;
        }
      }
    }
    for (auto& datagram : batch) sender_(std::move(datagram));
    batch.clear();
    sleeper.SleepMs(cfg_.quantumUs / 1000.0);
  }
}

// =============================================================================
// Wire1Channel
// =============================================================================

Wire1Channel::Wire1Channel(Context ctx) : ctx_(std::move(ctx)) {
  packetCfg_ = fec::PacketizerConfig::ForDatagramSize(1200);
  statWindowStartUs_ = HostMicroseconds();
}

Wire1Channel::~Wire1Channel() {
  {
    std::lock_guard<std::mutex> lock(m_);
    auth_ = Auth::Closed;
  }
  statsCv_.notify_one();
  if (statsThread_.joinable()) statsThread_.join();
  pacer_.Stop();
}

uint64_t Wire1Channel::HostMicroseconds() const {
  return QpcMicroseconds(QpcNow());
}

bool Wire1Channel::IsConnected() const {
  // Lock-free on purpose. The listener asks this while handling a Hello, and
  // that handler already holds m_; std::mutex is not recursive, and the second
  // lock would throw a deadlock error straight through a msquic callback.
  return peerConnected_ && linked_;
}

size_t Wire1Channel::BacklogBytes() const {
  std::lock_guard<std::mutex> lock(m_);
  return pacer_.BacklogBytes() + quic::GetDatagramStats(conn_).pendingBytes;
}

uint32_t Wire1Channel::UsableDatagramPayload() const {
  return std::min({peerMaxDatagram_ ? peerMaxDatagram_ : 1200u, 1400u,
                   quic::MaxDatagramPayload(conn_)});
}

void Wire1Channel::SendUrgent(std::vector<uint8_t> datagram) {
  if (mediaFlowBound_) quic::SendDatagram(conn_, std::move(datagram), true);
}

// ---- connection lifecycle ---------------------------------------------------

void Wire1Channel::OnConnected(QuicConn conn, const std::vector<uint8_t>& peerCertSha256) {
  {
    std::lock_guard<std::mutex> lk(m_);
    conn_ = conn;
    peerCertSha256_ = peerCertSha256;
    peerAddress_ = quic::PeerAddress(conn);
    peerName_.clear();
    auth_ = Auth::AwaitingHello;
    peerConnected_ = true;
    linked_ = true;
  }
  // The exporter secrets are the root of pairing and authentication (§7).
  quic::ExportKeyingMaterial(conn, kExporterSasLabel, sasExporter_);
  quic::ExportKeyingMaterial(conn, kExporterAuthLabel, authExporter_);
  quic::ExportKeyingMaterial(conn, kExporterPairLabel, pairExporter_);
  WVD_LOGI("quic: %s connected (exporters %s)", peerAddress_.c_str(),
           authExporter_.size() == 32 ? "ok" : "UNAVAILABLE");

}

void Wire1Channel::OnStream(QuicStream stream) {
  std::lock_guard<std::mutex> lk(m_);
  streams_[stream] = StreamState{};
}

void Wire1Channel::OnStreamClosed(QuicStream stream) {
  bool controlLost = false;
  {
    std::lock_guard<std::mutex> lk(m_);
    streams_.erase(stream);
    if (stream == inputStream_) inputStream_ = nullptr;
    if (stream == control_) {
      control_ = nullptr;
      controlLost = true;
    }
  }
  if (controlLost) Close();
}

void Wire1Channel::OnStreamData(QuicStream stream, const uint8_t* data, size_t len) {
  std::vector<msg::Envelope> envelopes;
  {
    std::lock_guard<std::mutex> lk(m_);
    auto it = streams_.find(stream);
    if (it == streams_.end()) it = streams_.emplace(stream, StreamState{}).first;
    if (!it->second.deframer.append(data, len)) {
      WVD_LOGW("quic: framing error on a peer stream");
      SendGoodbyeAndClose("invalid stream framing");
      return;
    }
    std::vector<uint8_t> body;
    while (it->second.deframer.next(body)) {
      msg::Envelope env;
      if (!env.decode(body.data(), body.size())) {
        WVD_LOGW("quic: malformed envelope");
        SendGoodbyeAndClose("invalid control envelope");
        return;
      }
      envelopes.push_back(std::move(env));
    }
  }
  for (auto& env : envelopes) RouteEnvelope(stream, env);
}

void Wire1Channel::RouteEnvelope(QuicStream stream, const msg::Envelope& env) {
  std::lock_guard<std::mutex> lk(m_);
  auto it = streams_.find(stream);
  if (it == streams_.end()) return;
  StreamState& st = it->second;
  if (!st.decided) {
    st.decided = true;
    if (env.kind == msg::Envelope::Kind::Hello && control_ == nullptr) {
      control_ = stream;
      st.isInput = false;
    } else if (env.kind == msg::Envelope::Kind::InputStreamOpen &&
               env.inputStreamOpen.sessionGen == ctx_.sessionGen && auth_ == Auth::Ready) {
      st.isInput = true;
      inputStream_ = stream;
      WVD_LOGI("quic: input stream opened (input_gen=%u)", inputGen_);
      return;
    } else {
      WVD_LOGW("quic: unexpected first envelope on a stream (kind %u) — stream closed",
               unsigned(env.kind));
      quic::CloseStream(stream, 1);
      streams_.erase(it);
      return;
    }
  }
  if (st.isInput) HandleInput(env);
  else HandleControl(env);
}

bool Wire1Channel::OnDatagram(const uint8_t* data, size_t len) {
  if (!len) return false;
  std::lock_guard<std::mutex> lock(m_);
  if (auth_ != Auth::Ready) return false;
  if (data[0] == kDgFlowHello) {
    dg::FlowHello hello;
    if (!dg::FlowHello::parse(data, len, hello) || hello.sessionGen != sessionGen() ||
        mediaFlowBound_ || UsableDatagramPayload() < kMinMediaDatagram)
      return false;
    mediaFlowBound_ = true;
    pacer_.Start([this](std::vector<uint8_t>&& datagram) {
      std::lock_guard<std::mutex> lock(m_);
      return conn_ && peerConnected_ && quic::SendDatagram(conn_, std::move(datagram));
    });
    SendSessionState();
    WVD_LOGI("quic: datagram flow bound to session %u (payload <= %u B)", sessionGen(),
             UsableDatagramPayload());
    return true;
  }
  if (data[0] == kDgNack && mediaFlowBound_) {
    dg::Nack nack;
    if (dg::Nack::parse(data, len, nack)) Retransmit(nack);
  }
  return false;
}

void Wire1Channel::OnClosed(uint32_t status) {
  bool was = false;
  {
    std::lock_guard<std::mutex> lk(m_);
    was = peerConnected_;
    peerConnected_ = false;
    linked_ = false;
    auth_ = Auth::Closed;
    conn_ = nullptr;
    inputStream_ = nullptr;
    control_ = nullptr;
    streams_.clear();
  }
  // The pacer must be quiet before the msquic handle goes away: it is closed
  // right after this callback returns.
  pacer_.Stop();
  statsCv_.notify_one();
  inboundCv_.notify_all();  // a pipeline waiting in RecvPacket must return
  if (was) WVD_LOGI("quic: connection closed (status 0x%x)", status);
}

void Wire1Channel::SetInputAllowed(bool allowed) {
  std::lock_guard<std::mutex> lk(m_);
  ctx_.cfg.inputAllowed = allowed;
  if (auth_ != Auth::Ready) return;
  if (!allowed) QueueInputRelease(kReleaseDisarm);
  SendSessionState();
}

void Wire1Channel::Close() {
  {
    std::lock_guard<std::mutex> lk(m_);
    peerConnected_ = false;
    linked_ = false;
    SendGoodbyeAndClose("host closed the session");
  }
  statsCv_.notify_one();
  inboundCv_.notify_all();
}

void Wire1Channel::SendGoodbyeAndClose(const std::string& reason) {
  msg::Envelope env;
  env.kind = msg::Envelope::Kind::Goodbye;
  env.goodbye.reason = reason;
  if (control_) SendEnvelopeOn(control_, env, streams_[control_]);
  if (conn_) quic::CloseConnection(conn_, 0);
  peerConnected_ = false;
  auth_ = Auth::Closed;
  statsCv_.notify_one();
  inboundCv_.notify_all();
}

// ---- envelopes --------------------------------------------------------------

void Wire1Channel::SendEnvelopeOn(QuicStream stream, const msg::Envelope& env, StreamState& st) {
  if (!stream) return;
  std::vector<uint8_t> bytes;
  if (!st.preambleSent) {
    bytes.insert(bytes.end(), framing::kPreamble, framing::kPreamble + 4);
    st.preambleSent = true;
  }
  auto framed = framing::frame(env.encode());
  bytes.insert(bytes.end(), framed.begin(), framed.end());
  if (!quic::SendStream(stream, bytes.data(), bytes.size()) && peerConnected_) {
    WVD_LOGW("quic: control stream send failed");
    peerConnected_ = false;
    auth_ = Auth::Closed;
    if (conn_) quic::CloseConnection(conn_, 0);
    statsCv_.notify_one();
    inboundCv_.notify_all();
  }
}

void Wire1Channel::SendEnvelope(const msg::Envelope& env) {
  auto it = streams_.find(control_);
  if (it == streams_.end()) return;
  SendEnvelopeOn(control_, env, it->second);
}

// ---- authentication and pairing (WIRE.md §7) --------------------------------

void Wire1Channel::HandleHello(const msg::Hello& hello) {
  if (auth_ != Auth::AwaitingHello) return;
  peerHello_ = hello;
  peerDeviceId_ = hello.deviceId;
  peerName_ = hello.deviceName.empty() ? (hello.deviceModel.empty() ? "headset"
                                                                    : hello.deviceModel)
                                       : hello.deviceName;
  peerModel_ = hello.deviceModel;
  peerMaxDatagram_ = hello.maxDatagramSize;
  thermal_ = hello.thermal;
  WVD_LOGI("quic: hello from '%s' (%s), device %s, max datagram %u B, decoder hevc=%u h264=%u",
           peerName_.c_str(), hello.deviceModel.c_str(), hello.deviceId.c_str(),
           hello.maxDatagramSize, hello.decoder.hevc ? 1u : 0u, hello.decoder.h264 ? 1u : 0u);

  msg::HelloAck ack;
  ack.protocolVersion = kWire1ProtocolVersion;
  ack.hostId = ctx_.cfg.hostId;
  ack.hostName = ctx_.cfg.hostName;
  ack.hostOs = ctx_.cfg.hostOs;
  ack.sessionGen = ctx_.sessionGen;
  ack.hostTimeUs = HostMicroseconds();

  auto sendAck = [&](msg::AuthResult result) {
    ack.auth = result;
    msg::Envelope out;
    out.kind = msg::Envelope::Kind::HelloAck;
    out.helloAck = ack;
    SendEnvelopeOn(control_, out, streams_[control_]);
  };

  if (hello.protocolVersion != kWire1ProtocolVersion) {
    WVD_LOGW("quic: peer speaks protocol %u, this host is %u — refused",
             hello.protocolVersion, kWire1ProtocolVersion);
    sendAck(msg::AuthResult::Rejected);
    SendGoodbyeAndClose("protocol version mismatch");
    return;
  }
  if (authExporter_.size() != 32) {
    // Without the exporter secrets nothing can be authenticated: fail closed.
    sendAck(msg::AuthResult::Rejected);
    SendGoodbyeAndClose("TLS exporter unavailable");
    return;
  }

  if (peerMaxDatagram_ != 0 && peerMaxDatagram_ < kMinMediaDatagram) {
    sendAck(msg::AuthResult::Rejected);
    SendGoodbyeAndClose("headset's datagram limit is too small");
    return;
  }
  statsThread_ = std::thread([this] { StatsLoop(); });

  if (!hello.authSignature.empty()) {
    auto device = ctx_.findDevice ? ctx_.findDevice(hello.deviceId) : std::nullopt;
    bool ok = device && device->publicKeyX963 == hello.devicePublicKey;
    if (ok) {
      const auto transcript = Transcript(AuthPurpose::Auth, authExporter_, ctx_.certSha256,
                                         hello.devicePublicKey, hello.deviceId);
      ok = VerifyDeviceSignature(hello.authSignature, transcript, hello.devicePublicKey);
    }
    if (!ok) {
      WVD_LOGW("quic: authentication failed for '%s' (%s)", peerName_.c_str(),
               hello.deviceId.c_str());
      sendAck(msg::AuthResult::Rejected);
      SendGoodbyeAndClose("authentication failed");
      return;
    }
    if (ctx_.claimActive && !ctx_.claimActive()) {
      sendAck(msg::AuthResult::Busy);
      SendGoodbyeAndClose("busy");
      return;
    }
    sendAck(msg::AuthResult::Ok);
    WVD_LOGI("quic: authenticated '%s' (paired)", peerName_.c_str());
    Authenticated();
    return;
  }

  if (hello.requestPairing) {
    if (!ctx_.cfg.pairingAllowed) {
      sendAck(msg::AuthResult::PairingRequired);
      SendGoodbyeAndClose("this host is not accepting new devices");
      return;
    }
    if (ctx_.claimActive && !ctx_.claimActive()) {
      sendAck(msg::AuthResult::Busy);
      SendGoodbyeAndClose("busy");
      return;
    }
    sendAck(msg::AuthResult::PairingStarted);
    if (!peerConnected_) return;

    auth_ = Auth::Pairing;
    pairingRequestId_ = nextPairingRequestId.fetch_add(1, std::memory_order_relaxed);
    pairingDeadlineUs_ = HostMicroseconds() + ctx_.cfg.pairingTimeoutMs * 1000ull;
    msg::Envelope challenge;
    challenge.kind = msg::Envelope::Kind::PairingChallenge;
    challenge.pairingChallenge.sasDigits = 6;
    challenge.pairingChallenge.timeoutMs = ctx_.cfg.pairingTimeoutMs;
    SendEnvelopeOn(control_, challenge, streams_[control_]);

    pairingSas_ = SasCode(sasExporter_, ctx_.certSha256, hello.devicePublicKey, 6);
    WVD_LOGI("PAIR %llu %s %s", (unsigned long long)pairingRequestId_, pairingSas_.c_str(),
             peerName_.c_str());
    if (ctx_.cfg.autoAcceptPairing) {
      WVD_LOGW("pairing accepted automatically (--pair-auto): the SAS check is skipped, so this "
               "only belongs on a trusted network");
      hostConfirmedPairing_ = true;
      MaybeCompletePairing();
    } else if (ctx_.observer) {
      ctx_.observer->OnPairingRequest(pairingRequestId_, peerName_, pairingSas_);
    }
    return;
  }

  sendAck(msg::AuthResult::Rejected);
  SendGoodbyeAndClose("no credentials offered");
}

void Wire1Channel::HandlePairingConfirm(const msg::PairingConfirm& confirm) {
  if (auth_ != Auth::Pairing) return;
  if (!confirm.accepted) {
    FinishPairing(false, "cancelled on the headset");
    return;
  }
  const auto transcript = Transcript(AuthPurpose::Pair, pairExporter_, ctx_.certSha256,
                                     peerHello_.devicePublicKey, peerHello_.deviceId);
  if (!VerifyDeviceSignature(confirm.deviceSignature, transcript,
                             peerHello_.devicePublicKey)) {
    FinishPairing(false, "invalid device signature");
    return;
  }
  clientConfirmedPairing_ = true;
  MaybeCompletePairing();
}

void Wire1Channel::MaybeCompletePairing() {
  if (clientConfirmedPairing_ && hostConfirmedPairing_) FinishPairing(true, "");
}

void Wire1Channel::DecidePairing(uint64_t requestId, bool accept) {
  std::lock_guard<std::mutex> lk(m_);
  if (auth_ != Auth::Pairing || requestId != pairingRequestId_) return;
  if (!accept) {
    FinishPairing(false, "rejected on the host");
    return;
  }
  hostConfirmedPairing_ = true;
  MaybeCompletePairing();
}

void Wire1Channel::FinishPairing(bool success, const std::string& reason) {
  if (pairingOutcomeSent_) return;
  pairingOutcomeSent_ = true;
  if (ctx_.observer) ctx_.observer->OnPairingDecided(pairingRequestId_, success);
  msg::Envelope out;
  out.kind = msg::Envelope::Kind::PairingResult;
  out.pairingResult.success = success;
  out.pairingResult.reason = success ? "" : reason;
  if (success) out.pairingResult.hostCertSha256 = ctx_.certSha256;
  SendEnvelopeOn(control_, out, streams_[control_]);
  if (!success) {
    WVD_LOGW("quic: pairing with '%s' failed: %s", peerName_.c_str(), reason.c_str());
    SendGoodbyeAndClose(reason);
    return;
  }
  if (ctx_.saveDevice) {
    PairedDevice device;
    device.deviceId = peerHello_.deviceId;
    device.name = peerHello_.deviceName;
    device.model = peerHello_.deviceModel;
    device.publicKeyX963 = peerHello_.devicePublicKey;
    ctx_.saveDevice(device);
  }
  WVD_LOGI("quic: paired with '%s' (%s)", peerName_.c_str(), peerHello_.deviceId.c_str());
  Authenticated();
}

void Wire1Channel::Authenticated() {
  if (!peerConnected_) return;
  auth_ = Auth::Ready;
  SendSessionState();
  statsCv_.notify_one();
}

void Wire1Channel::SendSessionState() {
  msg::Envelope out;
  out.kind = msg::Envelope::Kind::SessionState;
  out.sessionState.input =
      ctx_.cfg.inputAllowed ? msg::InputArmState::Armed : msg::InputArmState::Disarmed;
  out.sessionState.inputGen = inputGen_;
  out.sessionState.mediaFlowBound = mediaFlowBound_;
  SendEnvelopeOn(control_, out, streams_[control_]);
}

// ---- control-plane handlers -------------------------------------------------

void Wire1Channel::HandleControl(const msg::Envelope& env) {
  using K = msg::Envelope::Kind;
  switch (env.kind) {
    case K::Hello:
      HandleHello(env.hello);
      break;
    case K::PairingConfirm:
      HandlePairingConfirm(env.pairingConfirm);
      break;
    case K::Ping:
      SendPong(env.ping);
      break;
    case K::RecoveryRequest:
      if (auth_ == Auth::Ready)
        QueueRecovery(env.recoveryRequest.codecGen, uint32_t(env.recoveryRequest.kind),
                      env.recoveryRequest.lastGoodPicSeq);
      break;
    case K::ClientStats:
      if (auth_ == Auth::Ready) UpdateFecFromStats(env.clientStats);
      break;
    case K::StreamSettings:
      if (auth_ == Auth::Ready) {
        const bool audio = env.streamSettings.audio;
        if (audio != audioWanted_) {
          audioWanted_ = audio;
          QueueHello(audio, peerName_);
        }
        clientBitrateKbps_ = env.streamSettings.maxBitrateKbps;
      }
      break;
    case K::DisplayModeRequest:
      if (auth_ == Auth::Ready) QueueModeChange(env.displayModeRequest);
      break;
    case K::Visibility:
      // The headset shows or hides its window. Stopping the encoder would need
      // a media-session knob; until it exists we keep streaming and only
      // release held input, which a hidden monitor must not keep.
      if (auth_ == Auth::Ready && !env.visibility.visible) QueueInputRelease(kReleaseIdleTimeout);
      break;
    case K::ThermalUpdate:
      thermal_ = env.thermal.state;
      break;
    case K::Goodbye:
      WVD_LOGI("quic: headset said goodbye (%s)",
               env.goodbye.reason.empty() ? "no reason" : env.goodbye.reason.c_str());
      peerConnected_ = false;
      linked_ = false;
      auth_ = Auth::Closed;
      if (conn_) quic::CloseConnection(conn_, 0);
      statsCv_.notify_one();
      inboundCv_.notify_all();
      break;
    default:
      break;
  }
}

void Wire1Channel::HandleInput(const msg::Envelope& env) {
  if (auth_ != Auth::Ready) return;
  using K = msg::Envelope::Kind;
  switch (env.kind) {
    case K::InputEvents:
      if (!ctx_.cfg.inputAllowed) return;  // disarmed: events are dropped, not queued
      QueueInput(env.input);
      break;
    case K::InputRelease:
      QueueInputRelease(kReleaseDisarm);
      break;
    case K::Goodbye:
      QueueInputRelease(kReleaseDisconnect);
      break;
    default:
      break;
  }
}

void Wire1Channel::SendPong(const msg::Ping& ping) {
  msg::Envelope out;
  out.kind = msg::Envelope::Kind::Pong;
  out.pong.clientSendUs = ping.clientSendUs;
  out.pong.seq = ping.seq;
  const uint64_t now = HostMicroseconds();
  out.pong.hostRecvUs = now;
  out.pong.hostSendUs = now;
  auto it = streams_.find(control_);
  if (it != streams_.end()) SendEnvelopeOn(control_, out, it->second);
}

void Wire1Channel::UpdateFecFromStats(const msg::ClientStats& stats) {
  const double expected = double(stats.shardsReceived) + double(stats.shardsMissing);
  if (expected <= 0) return;
  const double loss = double(stats.shardsMissing) / expected;
  lossPercent_ = loss * 100.0;
  rttMs_ = double(stats.rttUs) / 1000.0;
  // Reference: ceil-ish loss*3 + 5%, clamped to 6..30%.
  packetCfg_.fecRatio = Clamp(0.05 + loss * 3.0, 0.06, 0.30);
}

// ---- outbound media ---------------------------------------------------------

bool Wire1Channel::Send(const uint8_t* data, size_t len) {
  auto pv = PacketView::Parse(data, len);
  if (!pv) return false;
  const PacketKind kind = pv->kind();

  std::unique_lock<std::mutex> lk(m_);
  if (!peerConnected_ || conn_ == nullptr) return false;
  switch (kind) {
    case PacketKind::MonitorLayout:
      OnLegacyLayout(pv->payload(), pv->payloadSize());
      break;
    case PacketKind::CodecConfig:
      OnLegacyCodecConfig(pv->payload(), pv->payloadSize());
      break;
    case PacketKind::DisplayModes:
      OnLegacyDisplayModes(pv->payload(), pv->payloadSize());
      break;
    case PacketKind::CursorShape: {
      auto h = pv->Get<CursorShapePayload>();
      if (!h) return false;
      const uint32_t available = h->dataSize <= pv->payloadSize() - sizeof(CursorShapePayload)
                                     ? h->dataSize
                                     : uint32_t(pv->payloadSize() - sizeof(CursorShapePayload));
      OnCursorShape(*h, pv->VarAt(uint32_t(sizeof(CursorShapePayload))), available);
      break;
    }
    case PacketKind::CursorPos: {
      auto cp = pv->Get<CursorPosPayload>();
      if (cp) OnCursorPos(*cp);
      break;
    }
    case PacketKind::Audio: {
      auto a = pv->Get<AudioPayload>();
      if (!a) return false;
      const size_t maxFrames = (pv->payloadSize() - sizeof(AudioPayload)) / sizeof(int16_t) /
                               (a->channels ? a->channels : 1);
      const uint32_t frames = (std::min)(uint32_t(maxFrames), a->frames);
      OnAudioPacket(*a, reinterpret_cast<const int16_t*>(pv->VarAt(uint32_t(sizeof(AudioPayload)))),
                    frames);
      break;
    }
    case PacketKind::Media: {
      auto mh = pv->Get<MediaPayloadHeader>();
      if (!mh) return false;
      const size_t maxBytes = pv->payloadSize() - sizeof(MediaPayloadHeader);
      const size_t bytes = (std::min)(size_t(mh->dataSize), maxBytes);
      OnMedia(*mh, pv->VarAt(uint32_t(sizeof(MediaPayloadHeader))), bytes, lk);
      break;
    }
    case PacketKind::Heartbeat:
      break;  // the headset pings every second; QUIC keeps itself alive
    default:
      break;  // host-originated packets the v1 wire has no equivalent for
  }
  return true;
}

void Wire1Channel::OnLegacyLayout(const uint8_t* payload, uint32_t size) {
  if (size < sizeof(MonitorLayoutPayload)) return;
  MonitorLayoutPayload ml{};
  std::memcpy(&ml, payload, sizeof(ml));
  std::vector<MonitorDesc> list;
  const uint32_t count = ml.count;
  if (count > 16 || size < sizeof(MonitorLayoutPayload) + count * sizeof(MonitorDesc)) return;
  list.resize(count);
  std::memcpy(list.data(), payload + sizeof(MonitorLayoutPayload),
              size_t(count) * sizeof(MonitorDesc));

  msg::Envelope out;
  out.kind = msg::Envelope::Kind::MonitorLayout;
  out.monitorLayout.layoutGen = uint32_t(ml.layoutGen);
  // A new layout invalidates the monitor we said the media belongs to.
  captureKnown_ = false;
  for (uint32_t i = 0; i < count; ++i) {
    msg::Monitor m;
    m.monitorId = i + 1;  // the headset numbers monitors by layout index + 1
    m.name = std::string(list[i].id, strnlen(list[i].id, sizeof(list[i].id)));
    m.width = list[i].width;
    m.height = list[i].height;
    // The packed layout carries no refresh rate; the headset's size menu reads
    // it from DisplayModes, which does, so this stays informational.
    m.refreshMhz = 60000;
    m.ppi = UINT(list[i].dpiX ? list[i].dpiX : 96);
    m.desktopX = list[i].x;
    m.desktopY = list[i].y;
    m.primary = list[i].primary != 0;
    out.monitorLayout.monitors.push_back(std::move(m));
  }
  layoutGenOut_ = out.monitorLayout.layoutGen;
  SendEnvelopeOn(control_, out, streams_[control_]);
  WVD_LOGI("quic: layout gen %u with %u monitor(s)", out.monitorLayout.layoutGen, count);
}

void Wire1Channel::OnLegacyCodecConfig(const uint8_t* payload, uint32_t size) {
  if (size < sizeof(CodecConfigPayload)) return;
  CodecConfigPayload cc{};
  std::memcpy(&cc, payload, sizeof(cc));
  std::vector<uint8_t> csd;
  if (cc.csdSize && size >= sizeof(CodecConfigPayload) + cc.csdSize) {
    csd.assign(payload + sizeof(CodecConfigPayload),
               payload + sizeof(CodecConfigPayload) + cc.csdSize);
  }
  msg::Envelope out;
  out.kind = msg::Envelope::Kind::CodecConfig;
  out.codecConfig.codecGen = uint32_t(cc.codecGen);
  out.codecConfig.codec = cc.codec == uint8_t(Codec::H264) ? msg::Codec::H264
                              : cc.codec == uint8_t(Codec::AV1) ? msg::Codec::Av1
                                                                : msg::Codec::Hevc;
  out.codecConfig.width = cc.width;
  out.codecConfig.height = cc.height;
  if (!csd.empty()) {
    size_t at = 0;
    while (true) {
      auto nal = NextParameterSet(csd.data(), csd.size(), at);
      if (nal.empty()) break;
      out.codecConfig.parameterSets.push_back(std::move(nal));
      if (out.codecConfig.parameterSets.size() >= 8) break;
    }
  }
  out.codecConfig.monitorId = captureMonitorId_;  // corrected when the first picture names it
  out.codecConfig.color.primaries = cc.vui.primaries;
  out.codecConfig.color.transfer = cc.vui.transfer;
  out.codecConfig.color.matrix = cc.vui.matrix;
  out.codecConfig.color.fullRange = cc.vui.fullRange != 0;
  out.codecConfig.color.chromaLocation = cc.vui.chromaLeft;
  out.codecConfig.nalLengthSize = 4;
  out.codecConfig.frameRateMhz =
      cc.fpsDen ? uint32_t((uint64_t(cc.fpsNum) * 1000u) / cc.fpsDen) : 60000u;
  frameIntervalUs_ =
      cc.fpsNum ? 1e6 * double(cc.fpsDen) / double(cc.fpsNum) : 1000000.0 / 60.0;
  targetBitrateBps_ = double(cc.bitrateKbps) * 1000.0;
  bitrateKbpsOut_ = cc.bitrateKbps;
  codecGenOut_ = uint32_t(cc.codecGen);
  history_.clear();
  historyBytes_ = 0;
  WVD_LOGI("quic: codec gen %u %s %ux%u, %zu parameter set(s), FEC %.0f%%",
           out.codecConfig.codecGen,
           out.codecConfig.codec == msg::Codec::H264 ? "H.264" : "HEVC", cc.width, cc.height,
           out.codecConfig.parameterSets.size(), packetCfg_.fecRatio * 100.0);
  pendingCodecConfig_ = std::move(out);
}

void Wire1Channel::OnLegacyDisplayModes(const uint8_t* payload, uint32_t size) {
  if (size < sizeof(DisplayModesPayload)) return;
  DisplayModesPayload p{};
  std::memcpy(&p, payload, sizeof(p));
  if (p.count > 64 || size < sizeof(DisplayModesPayload) + p.count * sizeof(DisplayModeDesc))
    return;
  std::vector<DisplayModeDesc> modes(p.count);
  std::memcpy(modes.data(), payload + sizeof(DisplayModesPayload),
              size_t(p.count) * sizeof(DisplayModeDesc));
  msg::Envelope out;
  out.kind = msg::Envelope::Kind::DisplayModes;
  out.displayModes.monitorId = p.monitor + 1;
  out.displayModes.layoutGen = uint32_t(p.layoutGen);
  out.displayModes.requestId = p.requestId;
  out.displayModes.status = msg::ModeChangeStatus(p.status);
  for (const auto& m : modes) {
    msg::DisplayMode dm;
    dm.width = m.width;
    dm.height = m.height;
    dm.refreshMhz = m.refreshMilliHz;
    dm.current = (m.flags & kModeCurrent) != 0;
    dm.native = (m.flags & kModeNative) != 0;
    dm.scaledForEncode = (m.flags & kModeScaledForEncode) != 0;
    out.displayModes.modes.push_back(dm);
  }
  SendEnvelopeOn(control_, out, streams_[control_]);
}

void Wire1Channel::OnCursorShape(const CursorShapePayload& shape, const uint8_t* pixels,
                                 uint32_t pixelBytes) {
  if (!shape.width || !shape.height || shape.width > 512 || shape.height > 512) return;
  if (pixelBytes < shape.width * shape.height * 4) return;
  msg::Envelope out;
  out.kind = msg::Envelope::Kind::CursorShape;
  out.cursorShape.monitorId = captureMonitorId_;
  out.cursorShape.shapeVer = shape.shapeVer;
  // The packed shape carries no layout generation; it is emitted with the
  // layout it belongs to, so the generation last sent out is the right one.
  out.cursorShape.layoutGen = layoutGenOut_;
  out.cursorShape.width = shape.width;
  out.cursorShape.height = shape.height;
  // The packed wire clamps hot spots; WIRE requires them inside the image.
  out.cursorShape.hotX = uint32_t(Clamp<int64_t>(shape.hotX, 0, shape.width - 1));
  out.cursorShape.hotY = uint32_t(Clamp<int64_t>(shape.hotY, 0, shape.height - 1));
  out.cursorShape.state = msg::CursorState::Separate;
  shapeVerSent_ = shape.shapeVer;
  // BGRA (packed) -> premultiplied RGBA (WIRE.md §3). A colour-keyed shape (a
  // mono cursor plus its transparent colour) becomes real alpha.
  std::vector<uint8_t> rgba(size_t(shape.width) * shape.height * 4);
  const bool keyed = shape.pixelFormat == 1;
  for (size_t i = 0; i < rgba.size(); i += 4) {
    const uint8_t b = pixels[i + 0], g = pixels[i + 1], r = pixels[i + 2], a = pixels[i + 3];
    if (keyed) {
      const uint32_t key = shape.colorKey;
      const uint32_t px = uint32_t(b) | (uint32_t(g) << 8) | (uint32_t(r) << 16) |
                          (uint32_t(a) << 24);
      if (px == key) {
        rgba[i + 0] = rgba[i + 1] = rgba[i + 2] = rgba[i + 3] = 0;
        continue;
      }
      rgba[i + 0] = r;
      rgba[i + 1] = g;
      rgba[i + 2] = b;
      rgba[i + 3] = a == 0 ? 255 : a;
    } else {
      rgba[i + 0] = r;
      rgba[i + 1] = g;
      rgba[i + 2] = b;
      rgba[i + 3] = a;
    }
  }
  out.cursorShape.rgba = std::move(rgba);
  if (!captureKnown_) pendingCursorShapes_.push_back(std::move(out));
  else SendEnvelopeOn(control_, out, streams_[control_]);
}

void Wire1Channel::OnCursorPos(const CursorPosPayload& cp) {
  if (!captureKnown_) return;  // no shape sent yet, so nothing to draw
  dg::CursorDatagram d;
  d.visible = cp.state != uint8_t(PointerState::Hidden);
  d.monitorId = uint16_t(cp.monitor + 1);
  d.layoutGen = uint32_t(cp.layoutGen);
  d.shapeVer = cp.shapeVer;
  d.seq = ++cursorSeq_;
  d.x = float(cp.x);
  d.y = float(cp.y);
  d.hostTimeUs = HostMicroseconds();
  SendUrgent(dg::CursorDatagram::encode(d));
}

void Wire1Channel::OnAudioPacket(const AudioPayload& a, const int16_t* pcm, uint32_t frames) {
  if (!mediaFlowBound_ || !audioWanted_ || !a.channels || !frames) return;
  // 5 ms packets, small enough for the path (WIRE.md §4.5).
  const uint32_t usable = UsableDatagramPayload();
  if (usable < kMinMediaDatagram) return;
  const uint32_t fit =
      uint32_t((usable - dg::AudioHeader::kHeaderSize) / (a.channels * sizeof(int16_t)));
  const uint32_t perPacket = (std::min)(a.sampleRate ? a.sampleRate / 200u : 240u, fit);
  if (!perPacket) return;
  const uint64_t packetTimeUs = a.qpcCapture > 0 ? QpcMicroseconds(a.qpcCapture) : HostMicroseconds();
  for (uint32_t off = 0; off < frames; off += perPacket) {
    const uint32_t n = (std::min)(perPacket, frames - off);
    std::vector<uint8_t> out(dg::AudioHeader::kHeaderSize +
                             size_t(n) * a.channels * sizeof(int16_t));
    dg::AudioHeader h;
    h.channels = uint8_t(a.channels);
    h.sessionGen = ctx_.sessionGen;
    h.seq = ++audioSeq_;
    h.sampleRate = a.sampleRate;
    h.hostTimeUs = packetTimeUs + (a.sampleRate ? uint64_t(off) * 1000000u / a.sampleRate : 0);
    h.frames = uint16_t(n);
    h.writeTo(out.data());
    memcpy(out.data() + dg::AudioHeader::kHeaderSize, pcm + size_t(off) * a.channels,
           size_t(n) * a.channels * sizeof(int16_t));
    SendUrgent(std::move(out));
  }
}

void Wire1Channel::OnMedia(const MediaPayloadHeader& mh, const uint8_t* au, size_t auBytes,
                           std::unique_lock<std::mutex>& lock) {
  if (!mediaFlowBound_ || !auBytes || mh.codecGen != codecGenOut_) return;
  const auto monitorId = uint16_t(mh.monitor + 1);
  captureMonitorId_ = monitorId;
  if (pendingCodecConfig_) {
    pendingCodecConfig_->codecConfig.monitorId = monitorId;
    SendEnvelope(*pendingCodecConfig_);
    pendingCodecConfig_.reset();
  }
  if (!captureKnown_) {
    captureKnown_ = true;
    for (auto& shape : pendingCursorShapes_) {
      shape.cursorShape.monitorId = monitorId;
      SendEnvelope(shape);
    }
    pendingCursorShapes_.clear();
  }
  const uint32_t usable = UsableDatagramPayload();
  if (usable < kMinMediaDatagram) return;
  auto packetConfig = fec::PacketizerConfig::ForDatagramSize(usable);
  packetConfig.fecRatio = packetCfg_.fecRatio;
  const int64_t captureQpc = mh.deadlineQpc - MsToQpc(double(ctx_.cfg.deadlineMs));
  const int64_t deadline = captureQpc + MsToQpc(ctx_.cfg.deadlineBudgetMs);
  if (QpcNow() >= deadline) {
    QueueRecovery(codecGenOut_, uint32_t(msg::RecoveryKind::Idr), 0);
    return;
  }
  fec::FrameDescriptor desc;
  desc.monitorId = uint16_t(mh.monitor + 1);
  desc.sessionGen = ctx_.sessionGen;
  desc.layoutGen = uint32_t(mh.layoutGen);
  desc.codecGen = uint32_t(mh.codecGen);
  desc.picSeq = mh.picSeq;
  desc.contentVer = mh.contentVer;
  desc.randomAccess = (mh.flags & 1u) != 0;
  desc.refinement = (mh.flags & 2u) != 0;
  desc.captureTimeUs = QpcMicroseconds((std::max)(int64_t(0), captureQpc));
  desc.deadlineBudgetMs = uint16_t(ctx_.cfg.deadlineBudgetMs);

  // Packetization and Reed-Solomon parity are CPU work. Keep the QUIC
  // receive path and the pacer's send path free while this picture is built.
  lock.unlock();
  fec::PacketizedFrame frame(desc, au, auBytes, packetConfig);
  frame.ComputeParity();
  auto datagrams = frame.interleavedData();
  frame.AppendInterleavedParity(datagrams);
  lock.lock();

  if (!peerConnected_ || mh.codecGen != codecGenOut_) return;
  const size_t pictureBytes = size_t(frame.totalDataShards()) *
                              (frame.shardBytes() + dg::MediaShardHeader::kSize);
  if (!pacer_.Enqueue(std::move(datagrams), frameIntervalUs_, targetBitrateBps_, deadline)) {
    QueueRecovery(codecGenOut_, uint32_t(msg::RecoveryKind::Idr), 0);
    return;
  }
  ++picturesOut_;
  if (desc.randomAccess) ++idrCount_;
  bytesOut_ += pictureBytes;

  const uint64_t now = HostMicroseconds();
  while (!history_.empty() &&
         (historyBytes_ + pictureBytes > kHistoryMaxBytes ||
          now - history_.begin()->second.descriptor().captureTimeUs >=
              uint64_t(ctx_.cfg.deadlineBudgetMs) * 1000)) {
    const auto& oldest = history_.begin()->second;
    historyBytes_ -= size_t(oldest.totalDataShards()) *
                     (oldest.shardBytes() + dg::MediaShardHeader::kSize);
    history_.erase(history_.begin());
  }
  if (pictureBytes <= kHistoryMaxBytes) {
    historyBytes_ += pictureBytes;
    history_.emplace(mh.picSeq, std::move(frame));
  }
}

void Wire1Channel::Retransmit(const dg::Nack& nack) {
  const auto it = history_.find(nack.picSeq);
  if (it == history_.end()) return;
  const auto& frame = it->second;
  const auto& desc = frame.descriptor();
  if (nack.codecGen != desc.codecGen || nack.monitorId != desc.monitorId) return;
  const uint64_t budgetUs = uint64_t(ctx_.cfg.deadlineBudgetMs) * 1000;
  const uint64_t ageUs = HostMicroseconds() - desc.captureTimeUs;
  if (ageUs >= budgetUs || frame.shardBytes() + dg::MediaShardHeader::kSize > UsableDatagramPayload())
    return;
  const int64_t deadline = QpcNow() + MsToQpc(double(budgetUs - ageUs) / 1000.0);
  auto shards = frame.Retransmission(nack.missing);
  const size_t count = shards.size();
  if (pacer_.Enqueue(std::move(shards), 2000.0, targetBitrateBps_, deadline))
    retransmitted_ += count;
}

void Wire1Channel::StatsLoop() {
  std::unique_lock<std::mutex> lock(m_);
  while (!statsCv_.wait_for(lock, std::chrono::seconds(1), [this] { return auth_ == Auth::Closed; })) {
    if (auth_ == Auth::Pairing && HostMicroseconds() > pairingDeadlineUs_)
      FinishPairing(false, "pairing timed out");
    if (auth_ != Auth::Ready || !mediaFlowBound_) continue;
    msg::Envelope out;
    out.kind = msg::Envelope::Kind::HostStats;
    const size_t pacerBacklog = pacer_.BacklogBytes();
    const uint64_t pacerDropped = pacer_.DroppedBytes();
    const uint64_t nowUs = HostMicroseconds();
    const double window =
        (std::max)(uint64_t(1), nowUs - statWindowStartUs_) / 1e6;
    out.hostStats.monitorId = captureMonitorId_;
    out.hostStats.fpsCaptured = 0;  // the pipeline reports this locally
    out.hostStats.fpsEncoded = uint32_t(double(picturesOut_ - lastStatsPictures_) / window);
    out.hostStats.bitrateKbps =
        uint32_t(double(bytesOut_ - lastStatsBytes_) * 8.0 / window / 1000.0);
    out.hostStats.targetBitrateKbps = bitrateKbpsOut_;
    out.hostStats.retransmittedShards = uint32_t(retransmitted_);
    out.hostStats.idrCount = idrCount_;
    msg::QueueDepth q;
    q.name = "pacer_bytes";
    q.depth = uint32_t(pacerBacklog);
    q.capacity = uint32_t(DatagramPacer::Config::maxQueuedBytes);
    q.dropped = pacerDropped;
    out.hostStats.queues.push_back(q);
    lastStatsPictures_ = picturesOut_;
    lastStatsBytes_ = bytesOut_;
    statWindowStartUs_ = nowUs;
    SendEnvelope(out);
  }
}

Wire1Channel::Link Wire1Channel::link() const {
  const uint64_t pacerDropped = pacer_.DroppedBytes();
  std::lock_guard<std::mutex> lk(m_);
  Link l;
  const auto transport = quic::GetDatagramStats(conn_);
  l.datagramsSent = transport.sent;
  l.retransmitted = retransmitted_;
  l.datagramDrops = transport.dropped;
  l.pacerDrops = pacerDropped;
  l.lossPercent = lossPercent_;
  l.rttMs = rttMs_;
  l.fecPercent = packetCfg_.fecRatio * 100.0;
  return l;
}

// ---- inbound: WIRE messages -> the packed packets the pipeline knows --------

void Wire1Channel::QueueForPipeline(std::vector<uint8_t> framedPacket) {
  if (!peerConnected_) return;
  {
    std::lock_guard<std::mutex> lk(inboundM_);
    inbound_.push_back(std::move(framedPacket));
    // Bounded: a pipeline that is not reading is a pipeline that is going away,
    // and stale events must not pile up behind it.
    while (inbound_.size() > 512) inbound_.pop_front();
  }
  inboundCv_.notify_all();
}

std::optional<std::vector<uint8_t>> Wire1Channel::RecvPacket(uint32_t timeoutMs) {
  std::unique_lock<std::mutex> lk(inboundM_);
  if (inbound_.empty()) {
    if (!peerConnected_) return std::nullopt;
    inboundCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                        [this] { return !inbound_.empty() || !peerConnected_; });
  }
  if (inbound_.empty()) return std::nullopt;
  auto out = std::move(inbound_.front());
  inbound_.pop_front();
  return out;
}

void Wire1Channel::QueueHello(bool wantsAudio, const std::string& deviceName) {
  PacketWriter w(PacketKind::Hello);
  HelloPayload h{};
  h.protocolVersion = kWireVersion;
  h.maxDecodedPixelsPerSec = peerHello_.decoder.maxPixelRate > 0xFFFFFFFFull
                                 ? 0xFFFFFFFFu
                                 : uint32_t(peerHello_.decoder.maxPixelRate);
  h.wantsAv1 = peerHello_.decoder.av1 ? 1 : 0;
  h.thermalState = uint8_t(thermal_);
  h.reserved = wantsAudio ? kHelloWantsAudio : 0;
  strncpy_s(h.deviceName, deviceName.c_str(), _TRUNCATE);
  w.Append(h);
  QueueForPipeline(w.Take());
}

void Wire1Channel::QueueRecovery(uint32_t codecGen, uint32_t kind, uint32_t refPicSeq) {
  PacketWriter w(PacketKind::RecoveryRequest);
  RecoveryRequestPayload r{};
  r.codecGen = codecGen;
  r.kind = kind;
  r.refPicSeq = refPicSeq;
  w.Append(r);
  QueueForPipeline(w.Take());
}

void Wire1Channel::QueueModeChange(const msg::DisplayModeRequest& r) {
  PacketWriter w(PacketKind::ModeChange);
  ModeChangeRequest m{};
  m.requestId = r.requestId;
  m.monitor = uint16_t(r.monitorId > 0 ? r.monitorId - 1 : 0);
  m.width = r.width;
  m.height = r.height;
  m.refreshMilliHz = r.refreshMhz;
  w.Append(m);
  captureMonitorId_ = uint16_t(r.monitorId > 0 ? r.monitorId : 1);
  QueueForPipeline(w.Take());
}

void Wire1Channel::QueueInputRelease(uint32_t reason) {
  PacketWriter w(PacketKind::InputRelease);
  InputReleasePayload r{};
  r.inputGen = inputGen_;
  r.reason = reason;
  w.Append(r);
  QueueForPipeline(w.Take());
}

// WIRE input events -> the packed InputEvent the privileged helper injects.
void Wire1Channel::QueueInput(const msg::InputEvents& batch) {
  std::vector<InputEvent> events;
  events.reserve((std::min)(size_t(256), batch.events.size()));
  for (const auto& e : batch.events) {
    InputEvent out{};
    switch (e.kind) {
      case msg::InputEvent::Kind::Move: {
        const auto& mv = e.move;
        if (!std::isfinite(mv.x) || !std::isfinite(mv.y) || mv.x < 0 || mv.y < 0) continue;
        out.kind = uint8_t(InputKind::MoveAbs);
        out.x = int32_t(mv.x);
        out.y = int32_t(mv.y);
        out.monitor = uint16_t(mv.monitorId > 0 ? mv.monitorId - 1 : 0);
        break;
      }
      case msg::InputEvent::Kind::Button: {
        const auto& b = e.button;
        if (!std::isfinite(b.x) || !std::isfinite(b.y) || b.x < 0 || b.y < 0) continue;
        out.kind = uint8_t(InputKind::Button);
        out.flags = uint8_t(b.down ? 0 : kKeyUp);
        if (b.clickCount > 1) out.flags |= kButtonDblclk;
        out.code = uint16_t(1u << Clamp<uint32_t>(uint32_t(b.button), 0u, 2u));
        out.x = int32_t(b.x);
        out.y = int32_t(b.y);
        out.monitor = uint16_t(b.monitorId > 0 ? b.monitorId - 1 : 0);
        break;
      }
      case msg::InputEvent::Kind::Scroll: {
        const auto& s = e.scroll;
        if (!std::isfinite(s.dy)) continue;
        out.kind = uint8_t(InputKind::Wheel);
        const double notch = s.precise ? 3.0 : 120.0;
        out.wheelDelta = int32_t(Clamp(s.dy * notch, -10000.0, 10000.0));
        break;
      }
      case msg::InputEvent::Kind::Key: {
        const auto& k = e.key;
        if (k.hidUsage > 0xFFFF) continue;
        out.kind = uint8_t(InputKind::Key);
        out.code = uint16_t(k.hidUsage);
        out.flags = uint8_t(k.down ? 0 : kKeyUp);
        break;
      }
      case msg::InputEvent::Kind::Text:
        // No Unicode injection event on this wire (never reinterpret text as
        // HID usages); the headset's text arrives as key events.
        continue;
      default:
        continue;
    }
    events.push_back(out);
    if (events.size() >= 256) break;
  }
  if (events.empty()) return;
  PacketWriter w(PacketKind::Input, ++inputSeq_);
  InputPayload hdr{};
  hdr.inputGen = inputGen_;
  hdr.seq = inputSeq_;
  hdr.count = uint32_t(events.size());
  w.Append(hdr);
  w.AppendArray(events);
  QueueForPipeline(w.Take());
}

// =============================================================================
// Wire1Listener
// =============================================================================

std::string Wire1HostId(const std::vector<uint8_t>& certSha256) {
  static const char* hex = "0123456789abcdef";
  std::string id = "wvm-";
  for (size_t i = 0; i < certSha256.size() && i < 8; ++i) {
    id.push_back(hex[certSha256[i] >> 4]);
    id.push_back(hex[certSha256[i] & 0xF]);
  }
  return id;
}

Wire1Listener::Wire1Listener(GenCounters& gens, Wire1Config cfg, HostIdentity& identity,
                             PairedDeviceStore& devices)
    : gens_(gens), cfg_(std::move(cfg)), identity_(identity), devices_(devices) {}

Wire1Listener::~Wire1Listener() { Stop(); }

bool Wire1Listener::Listen(uint16_t port, std::string* error, bool loopbackOnly) {
  QuicServerOptions options;
  options.port = port;
  options.loopbackOnly = loopbackOnly;
  return server_.Start(options, identity_, *this, error);
}

void Wire1Listener::Stop() {
  {
    std::lock_guard<std::mutex> lk(m_);
    stopping_ = true;
  }
  server_.Stop();
  cv_.notify_all();
}

std::shared_ptr<Wire1Channel> Wire1Listener::Lookup(QuicConn conn) {
  std::lock_guard<std::mutex> lk(m_);
  auto it = channels_.find(conn);
  return it == channels_.end() ? nullptr : it->second;
}

void Wire1Listener::OnQuicConnected(QuicConn conn, const std::vector<uint8_t>& peerCertSha256) {
  Wire1Channel::Context ctx;
  ctx.gens = &gens_;
  ctx.cfg = cfg_;
  ctx.certSha256 = identity_.certSha256();
  ctx.observer = observer_;
  ctx.findDevice = [this](const std::string& id) -> std::optional<PairedDevice> {
    std::lock_guard<std::mutex> lock(devicesMutex_);
    if (const auto* device = devices_.find(id)) return *device;
    return std::nullopt;
  };
  ctx.saveDevice = [this](const PairedDevice& device) {
    std::lock_guard<std::mutex> lock(devicesMutex_);
    devices_.save(device);
    std::string error;
    if (!devices_.Save(&error))
      WVD_LOGE("pairing: cannot persist the device list: %s", error.c_str());
  };
  {
    std::lock_guard<std::mutex> lk(m_);
    ctx.sessionGen = uint32_t(gens_.sessionGen.fetch_add(1) + 1);
    channels_[conn] = nullptr;  // placeholder until the channel exists
  }
  auto channel = std::make_shared<Wire1Channel>(std::move(ctx));
  std::weak_ptr<Wire1Channel> self = channel;
  // One streaming session per host (WIRE.md §1): whoever authenticates first
  // holds the slot, and a newer session replaces a stale one.
  channel->SetClaimActive([this, self] {
    const auto me = self.lock();
    if (!me) return false;
    std::lock_guard<std::mutex> lk(m_);
    auto current = active_.lock();
    if (current && current != me && current->IsConnected()) return false;
    active_ = me;
    return true;
  });
  {
    std::lock_guard<std::mutex> lk(m_);
    channels_[conn] = channel;
  }
  channel->OnConnected(conn, peerCertSha256);
}

std::shared_ptr<Wire1Channel> Wire1Listener::active() const {
  std::lock_guard<std::mutex> lk(m_);
  return active_.lock();
}

void Wire1Listener::OnQuicStream(QuicConn conn, QuicStream stream) {
  auto channel = Lookup(conn);
  if (!channel) return;
  channel->OnStream(stream);
}

void Wire1Listener::OnQuicStreamData(QuicConn conn, QuicStream stream, const uint8_t* data,
                                     size_t len) {
  auto channel = Lookup(conn);
  if (!channel) return;
  channel->OnStreamData(stream, data, len);

}

void Wire1Listener::OnQuicStreamClosed(QuicConn conn, QuicStream stream) {
  auto channel = Lookup(conn);
  if (channel) channel->OnStreamClosed(stream);
}

void Wire1Listener::OnQuicDatagram(QuicConn conn, const uint8_t* data, size_t len) {
  auto channel = Lookup(conn);
  if (channel && channel->OnDatagram(data, len)) {
    std::lock_guard<std::mutex> lock(m_);
    if (!stopping_) ready_.push_back(std::move(channel));
    cv_.notify_one();
  }
}

void Wire1Listener::OnQuicClosed(QuicConn conn, uint32_t status) {
  std::shared_ptr<Wire1Channel> channel;
  {
    std::lock_guard<std::mutex> lk(m_);
    auto it = channels_.find(conn);
    if (it == channels_.end()) return;
    channel = it->second;
    channels_.erase(it);
    for (auto r = ready_.begin(); r != ready_.end();) {
      if (*r == channel) r = ready_.erase(r);
      else ++r;
    }
    if (active_.lock() == channel) active_.reset();
  }
  if (channel) channel->OnClosed(status);
  cv_.notify_all();
}

std::shared_ptr<Wire1Channel> Wire1Listener::AcceptChannel(uint32_t timeoutMs) {
  std::unique_lock<std::mutex> lk(m_);
  cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
               [this] { return stopping_ || !ready_.empty(); });
  if (stopping_) return nullptr;
  while (!ready_.empty()) {
    auto channel = ready_.front();
    ready_.pop_front();
    if (channel->IsConnected()) {
      auto previous = active_.lock();
      active_ = channel;
      if (previous && previous != channel && previous->IsConnected()) {
        // A newer headset took over: the old session ends.
        lk.unlock();
        previous->Close();
        lk.lock();
      }
      return channel;
    }
  }
  return nullptr;
}

void Wire1Listener::DecidePairing(uint64_t requestId, bool accept) {
  std::vector<std::shared_ptr<Wire1Channel>> snapshot;
  {
    std::lock_guard<std::mutex> lk(m_);
    snapshot.reserve(channels_.size());
    for (auto& c : channels_)
      if (c.second) snapshot.push_back(c.second);
  }
  for (auto& c : snapshot) c->DecidePairing(requestId, accept);
}

}  // namespace wvd

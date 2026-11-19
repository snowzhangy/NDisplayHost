// Loopback regression test for the QUIC host (proto/WIRE.md).
//
// A real msquic client, in the same process as the real Wire1Listener, does
// what a Vision Pro does: pairs with a SAS check, binds the datagram flow,
// and receives a coded picture as FEC-protected datagrams. Everything on the
// host side is production code — the listener, the pairing state machine, the
// envelope codec, the packetiser, the pacer, the NACK history and the
// packed-packet translation — so this harness catches a broken wire before a
// headset is anywhere near it.
//
// WVM_HOME points at a throwaway directory: a test must never pair into the
// machine's real device list.
#include <windows.h>
#include <bcrypt.h>
#include <cstdio>
#include <cstdlib>

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "identity.h"
#include "wdauth.h"
#include "wire1_channel.h"
#include "wvd/fec.h"
#include "wvd/logging.h"
#include "wvd/protocol.h"
#include "wvd/pacing.h"
#include "wvd/time.h"
#include "wvd/wire1.h"

#pragma comment(lib, "bcrypt.lib")

using namespace wvd;

static int g_failures = 0;

#define CHECK(cond, ...)              \
  do {                                \
    if (!(cond)) {                    \
      std::printf("FAIL %s:%d ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);       \
      std::printf("\n");              \
      ++g_failures;                   \
    }                                 \
  } while (0)

namespace {

// ---- the headset's key ------------------------------------------------------

struct TestKey {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_KEY_HANDLE key = nullptr;
  std::vector<uint8_t> x963;  // 0x04 || X || Y
  std::string why;

  bool Create() {
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0))) {
      why = "open ECDSA P-256";
      return false;
    }
    if (!BCRYPT_SUCCESS(BCryptGenerateKeyPair(alg, &key, 256, 0))) {
      why = "generate key pair";
      return false;
    }
    if (!BCRYPT_SUCCESS(BCryptFinalizeKeyPair(key, 0))) {
      why = "finalize key pair";
      return false;
    }
    ULONG len = 0;
    if (!BCRYPT_SUCCESS(BCryptExportKey(key, 0, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &len, 0))) {
      why = "public blob size";
      return false;
    }
    std::vector<uint8_t> blob(len);
    if (!BCRYPT_SUCCESS(BCryptExportKey(key, 0, BCRYPT_ECCPUBLIC_BLOB, blob.data(), len, &len, 0))) {
      why = "export public blob";
      return false;
    }
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    x963.clear();
    x963.push_back(0x04);
    x963.insert(x963.end(), blob.begin() + long(sizeof(BCRYPT_ECCKEY_BLOB)),
                blob.begin() + long(sizeof(BCRYPT_ECCKEY_BLOB) + header->cbKey * 2));
    if (x963.size() != 65) {
      why = "x963 is " + std::to_string(x963.size()) + " bytes (blob " + std::to_string(blob.size()) +
            ", cbKey " + std::to_string(header->cbKey) + ", first bytes "
            + [&] {
                std::string h;
                for (size_t i = 0; i < 16 && i < blob.size(); ++i) {
                  char b[8];
                  _snprintf_s(b, sizeof(b), _TRUNCATE, "%02x ", blob[i]);
                  h += b;
                }
                return h;
              }() + ")";
      return false;
    }
    return true;
  }

  // ECDSA P-256/SHA-256 over `message`, DER encoded (what the host verifies).
  std::vector<uint8_t> Sign(const std::vector<uint8_t>& message) {
    const std::vector<uint8_t> hash = Sha256(message.data(), message.size());
    uint8_t raw[64] = {};
    ULONG sigLen = 0;
    if (!BCRYPT_SUCCESS(
            BCryptSignHash(key, nullptr, const_cast<PUCHAR>(hash.data()), ULONG(hash.size()), raw,
                           sizeof(raw), &sigLen, 0)))
      return {};
    return EcdsaRawToDer(raw, 32);
  }
};

// ---- observer: what the operator would see ----------------------------------

class Observer : public IWire1Observer {
 public:
  struct Request {
    uint64_t id = 0;
    std::string name;
    std::string sas;
  };
  void OnPairingRequest(uint64_t requestId, const std::string& deviceName,
                        const std::string& sas) override {
    std::lock_guard<std::mutex> lk(m);
    pending.push_back(Request{requestId, deviceName, sas});
    cv.notify_all();
  }
  void OnPairingDecided(uint64_t requestId, bool accepted) override {
    std::lock_guard<std::mutex> lk(m);
    decided.emplace_back(requestId, accepted);
    cv.notify_all();
  }
  mutable std::mutex m;
  std::condition_variable cv;
  std::vector<Request> pending;
  std::vector<std::pair<uint64_t, bool>> decided;
};

// ---- the headset's connection ----------------------------------------------

class FakeHeadset : public IQuicSink {
 public:
  void OnQuicConnected(QuicConn conn, const std::vector<uint8_t>& peerCertSha256) override {
    {
      std::lock_guard<std::mutex> lk(m_);
      conn_ = conn;
      peerCert_ = peerCertSha256;
      connected_ = true;
    }
    cv_.notify_all();
  }
  void OnQuicStream(QuicConn, QuicStream stream) override {
    Touch(stream);
  }
  void OnQuicStreamData(QuicConn, QuicStream stream, const uint8_t* data, size_t len) override {
    Touch(stream);
    std::vector<msg::Envelope> out;
    {
      std::lock_guard<std::mutex> lk(m_);
      auto it = streams_.find(stream);
      // Append once, then drain: appending inside the drain loop would feed
      // the same bytes back in and break the framing after the first frame.
      if (it->second.append(data, len)) {
        std::vector<uint8_t> body;
        while (it->second.next(body)) {
          msg::Envelope env;
          if (env.decode(body.data(), body.size())) out.push_back(std::move(env));
        }
      }
      for (auto& e : out) envelopes_.push_back(std::move(e));
    }
    cv_.notify_all();
  }
  void OnQuicStreamClosed(QuicConn, QuicStream stream) override {
    std::lock_guard<std::mutex> lk(m_);
    streams_.erase(stream);
  }
  void OnQuicDatagram(QuicConn, const uint8_t* data, size_t len) override {
    {
      std::lock_guard<std::mutex> lk(m_);
      datagrams_.emplace_back(data, data + len);
    }
    cv_.notify_all();
  }
  void OnQuicClosed(QuicConn, uint32_t) override {
    {
      std::lock_guard<std::mutex> lk(m_);
      connected_ = false;
    }
    cv_.notify_all();
  }

  std::vector<uint8_t> peerCertSha256() const {
    std::lock_guard<std::mutex> lk(m_);
    return peerCert_;
  }
  QuicConn conn() const {
    std::lock_guard<std::mutex> lk(m_);
    return conn_;
  }
  // A wait predicate runs while wait_for holds m_, so it must not lock.
  bool WaitConnected(uint32_t ms) { return Wait(ms, [this] { return connected_; }); }
  bool WaitClosed(uint32_t ms) { return Wait(ms, [this] { return !connected_; }); }

  // Waits for the next unseen envelope of `kind`.
  bool WaitForEnvelope(msg::Envelope::Kind kind, uint32_t ms, msg::Envelope* out) {
    std::unique_lock<std::mutex> lk(m_);
    size_t& cursor = cursors_[unsigned(kind)];
    return cv_.wait_for(lk, std::chrono::milliseconds(ms), [&] {
      for (size_t i = cursor; i < envelopes_.size(); ++i) {
        if (envelopes_[i].kind == kind) {
          if (out) *out = envelopes_[i];
          cursor = i + 1;
          return true;
        }
      }
      return false;
    });
  }

  // Moves out every buffered datagram whose first byte is `type`, waiting at
  // least until one arrives (or the timeout).
  bool DrainDatagrams(uint8_t type, uint32_t ms, std::vector<std::vector<uint8_t>>* out) {
    std::unique_lock<std::mutex> lk(m_);
    const bool ok = cv_.wait_for(lk, std::chrono::milliseconds(ms), [&] {
      for (const auto& d : datagrams_)
        if (!d.empty() && d[0] == type) return true;
      return false;
    });
    for (auto it = datagrams_.begin(); it != datagrams_.end();) {
      if (!it->empty() && (*it)[0] == type) {
        out->push_back(std::move(*it));
        it = datagrams_.erase(it);
      } else {
        ++it;
      }
    }
    return ok;
  }

  template <class Pred>
  bool Wait(uint32_t ms, Pred pred) {
    std::unique_lock<std::mutex> lk(m_);
    return cv_.wait_for(lk, std::chrono::milliseconds(ms), pred);
  }

  void SendEnvelope(QuicStream stream, const msg::Envelope& env) {
    std::vector<uint8_t> bytes;
    {
      std::lock_guard<std::mutex> lk(m_);
      if (!preambles_.count(stream)) {
        bytes.insert(bytes.end(), framing::kPreamble, framing::kPreamble + 4);
        preambles_.insert(stream);
      }
    }
    auto framed = framing::frame(env.encode());
    bytes.insert(bytes.end(), framed.begin(), framed.end());
    quic::SendStream(stream, bytes.data(), bytes.size());
  }

 private:
  void Touch(QuicStream stream) {
    std::lock_guard<std::mutex> lk(m_);
    if (!streams_.count(stream)) streams_.emplace(stream, framing::Deframer{true});
  }

  mutable std::mutex m_;
  std::condition_variable cv_;
  QuicConn conn_ = nullptr;
  bool connected_ = false;
  std::vector<uint8_t> peerCert_;
  std::map<QuicStream, framing::Deframer> streams_;
  std::set<QuicStream> preambles_;
  std::vector<msg::Envelope> envelopes_;
  std::vector<std::vector<uint8_t>> datagrams_;
  size_t cursors_[32] = {};
};

// ---- packed packets, as the capture/encode pipeline would produce them ------

std::vector<uint8_t> PackedLayout() {
  PacketWriter w(PacketKind::MonitorLayout);
  MonitorLayoutPayload ml{};
  ml.count = 1;
  ml.layoutGen = 7;
  w.Append(ml);
  MonitorDesc d{};
  d.width = 1920;
  d.height = 1080;
  d.dpiX = 141;
  d.dpiY = 141;
  d.primary = 1;
  strncpy_s(d.id, "\\\\.\\DISPLAY1", _TRUNCATE);
  w.Append(d);
  return w.Take();
}

std::vector<uint8_t> PackedCodecConfig(uint64_t codecGen) {
  PacketWriter w(PacketKind::CodecConfig);
  CodecConfigPayload cc{};
  cc.codecGen = codecGen;
  cc.width = 1920;
  cc.height = 1080;
  cc.codec = uint8_t(Codec::HEVC);
  cc.fpsNum = 60000;
  cc.fpsDen = 1000;
  cc.bitrateKbps = 24000;
  // Two parameter sets with start codes: WIRE wants them bare.
  const uint8_t csd[] = {0, 0, 0, 1, 0x40, 0x01, 0x0c, 0x01,
                         0, 0, 0, 1, 0x42, 0x01, 0x02, 0x03};
  cc.csdSize = sizeof(csd);
  w.Append(cc);
  w.AppendBytes(csd, sizeof(csd));
  return w.Take();
}

// A deterministic "coded picture" so the receiver can verify the bytes.
uint8_t AuByte(uint32_t picSeq, size_t i) { return uint8_t((i * 31u + picSeq * 7u) & 0xFFu); }

std::vector<uint8_t> PackedMedia(uint32_t picSeq, uint64_t codecGen, uint32_t bytes, bool idr) {
  PacketWriter w(PacketKind::Media);
  MediaPayloadHeader mh{};
  mh.picSeq = picSeq;
  mh.contentVer = picSeq;
  mh.flags = idr ? 1u : 0u;
  mh.codecGen = codecGen;
  mh.layoutGen = 7;
  mh.deadlineQpc = QpcNow() + MsToQpc(50);
  mh.monitor = 0;
  mh.dataSize = bytes;
  w.Append(mh);
  std::vector<uint8_t> au(bytes);
  for (size_t i = 0; i < au.size(); ++i) au[i] = AuByte(picSeq, i);
  w.AppendBytes(au.data(), au.size());
  return w.Take();
}

std::vector<uint8_t> FlowHelloBytes(uint32_t sessionGen) {
  std::vector<uint8_t> p(dg::FlowHello::kSize, 0);
  p[0] = kDgFlowHello;
  const uint32_t magic = dg::FlowHello::kMagic;
  memcpy(p.data() + 4, &magic, 4);
  memcpy(p.data() + 8, &sessionGen, 4);
  return p;
}

// Optional local throughput sample. Control and media traverse the same
// Schannel/MsQuic path as the regression above. Encoder time is excluded.
void MeasureDelivery(Wire1Channel& channel, FakeHeadset& headset, uint32_t codecGen) {
  constexpr uint32_t kFrames = 120, kBytesPerFrame = 125000, kFirstPicture = 1000;
  size_t received = 0, maxBacklog = 0;
  const auto before = channel.link();
  auto drain = [&] {
    std::vector<std::vector<uint8_t>> batch;
    headset.DrainDatagrams(kDgMediaShard, 0, &batch);
    for (const auto& datagram : batch) {
      dg::MediaShardHeader h{};
      if (!dg::MediaShardHeader::parse(datagram.data(), datagram.size(), h) ||
          h.picSeq < kFirstPicture || h.flags & dg::MediaShardHeader::kParity)
        continue;
      const uint32_t offset = uint32_t(h.blockFirstShard + h.shardIndex) * h.shardBytes;
      received += (std::min)(uint32_t(h.shardBytes), h.frameBytes - offset);
    }
  };
  PrecisionSleeper sleeper;
  const auto start = QpcNow();
  for (uint32_t i = 0; i < kFrames; ++i) {
    sleeper.SleepUntilQpc(start + MsToQpc(i * (1000.0 / 60)));
    channel.SendPacket(PackedMedia(kFirstPicture + i, codecGen, kBytesPerFrame, i == 0));
    maxBacklog = (std::max)(maxBacklog, channel.BacklogBytes());
    drain();
  }
  sleeper.SleepUntilQpc(start + MsToQpc(2100));
  drain();
  const auto link = channel.link();
  std::printf("QUIC loopback at 60 Mbit/s offered: %.2f%% payload delivered, "
              "%zu B peak send backlog, %llu transport drops, %llu B expired/rejected\n",
              received * 100.0 / (size_t(kFrames) * kBytesPerFrame), maxBacklog,
              static_cast<unsigned long long>(link.datagramDrops - before.datagramDrops),
              static_cast<unsigned long long>(link.pacerDrops - before.pacerDrops));
}

bool Run(bool measure) {
  wchar_t tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  const std::wstring home = std::wstring(tmp) + L"wvm-quic-test-" + std::to_wstring(GetTickCount());
  SetEnvironmentVariableW(L"WVM_HOME", home.c_str());

  std::string error;
  if (!QuicSharedOpen(&error)) {
    std::printf("FAIL %s\n", error.c_str());
    return false;
  }
  HostIdentity identity;
  if (!identity.Load("WVM test host", &error)) {
    std::printf("FAIL host identity: %s\n", error.c_str());
    return false;
  }
  CHECK(identity.certSha256().size() == 32, "certificate hash");

  PairedDeviceStore devices;
  GenCounters gens;
  Wire1Config cfg;
  cfg.hostId = Wire1HostId(identity.certSha256());
  cfg.hostName = "test";
  cfg.pairingAllowed = true;
  cfg.inputAllowed = true;
  // The SAS is normally compared by a human; here the test compares it, so
  // auto-accept stays off and the operator path is exercised too.
  cfg.autoAcceptPairing = false;

  Wire1Listener listener(gens, cfg, identity, devices);
  Observer observer;
  listener.SetObserver(&observer);
  if (!listener.Listen(0, &error, /*loopbackOnly=*/true)) {
    std::printf("FAIL listen: %s\n", error.c_str());
    return false;
  }

  FakeHeadset headset;
  QuicClient client;
  if (!client.Connect("127.0.0.1", listener.port(), headset, &error, identity.certSha256())) {
    std::printf("FAIL connect: %s\n", error.c_str());
    return false;
  }
  if (!headset.WaitConnected(8000)) {
    std::printf("FAIL the TLS handshake did not complete\n");
    return false;
  }
  CHECK(headset.peerCertSha256() == identity.certSha256(), "the client pinned the wrong host");

  TestKey key;
  if (!key.Create()) {
    std::printf("FAIL test device key: %s\n", key.why.c_str());
    return false;
  }

  // ---- pairing, with the confirmation code compared on both sides ----------
  QuicStream control = quic::OpenStream(client.conn());
  CHECK(control != nullptr, "the control stream could not be opened");
  msg::Envelope hello;
  hello.kind = msg::Envelope::Kind::Hello;
  hello.hello.protocolVersion = 1;
  hello.hello.deviceId = "test-headset";
  hello.hello.deviceName = "Test Vision Pro";
  hello.hello.deviceModel = "visionOS";
  hello.hello.devicePublicKey = key.x963;
  hello.hello.requestPairing = true;
  hello.hello.maxDatagramSize = 1100;
  hello.hello.decoder.hevc = true;
  hello.hello.decoder.maxPixelRate = 3840ull * 2160ull * 60ull;
  headset.SendEnvelope(control, hello);

  msg::Envelope ack;
  CHECK(headset.WaitForEnvelope(msg::Envelope::Kind::HelloAck, 8000, &ack), "no HelloAck");
  CHECK(ack.helloAck.auth == msg::AuthResult::PairingStarted, "pairing not started (result %u)",
        unsigned(ack.helloAck.auth));
  CHECK(ack.helloAck.hostId == cfg.hostId, "host id");
  const uint32_t sessionGen = ack.helloAck.sessionGen;

  msg::Envelope challenge;
  CHECK(headset.WaitForEnvelope(msg::Envelope::Kind::PairingChallenge, 8000, &challenge),
        "no PairingChallenge");

  // The code the host shows must equal the one the headset derives from its own
  // exporter secret: that is the whole man-in-the-middle argument (§7.2).
  std::vector<uint8_t> sasExporter;
  CHECK(quic::ExportKeyingMaterial(client.conn(), kExporterSasLabel, sasExporter),
        "the client cannot export the SAS secret");
  const std::string clientSas = SasCode(sasExporter, identity.certSha256(), key.x963, 6);

  bool prompted = false;
  {
    std::unique_lock<std::mutex> lk(observer.m);
    prompted = observer.cv.wait_for(lk, std::chrono::seconds(8),
                                   [&] { return !observer.pending.empty(); });
    if (prompted)
      CHECK(observer.pending.front().sas == clientSas, "host SAS %s != headset SAS %s",
            observer.pending.front().sas.c_str(), clientSas.c_str());
    CHECK(observer.pending.front().name == "Test Vision Pro", "device name in the prompt");
  }
  CHECK(prompted, "the host never asked the operator");
  if (prompted) listener.DecidePairing(observer.pending.front().id, true);

  // The device confirms with a signature over the pairing transcript; without
  // it the host would wait for the timeout, so send it like a real client.
  std::vector<uint8_t> pairExporter;
  CHECK(quic::ExportKeyingMaterial(client.conn(), kExporterPairLabel, pairExporter),
        "the client cannot export the pairing secret");
  const std::vector<uint8_t> pairTranscript =
      Transcript(AuthPurpose::Pair, pairExporter, identity.certSha256(), key.x963, "test-headset");
  msg::Envelope confirm;
  confirm.kind = msg::Envelope::Kind::PairingConfirm;
  confirm.pairingConfirm.accepted = true;
  confirm.pairingConfirm.deviceSignature = key.Sign(pairTranscript);
  headset.SendEnvelope(control, confirm);

  msg::Envelope result;
  CHECK(headset.WaitForEnvelope(msg::Envelope::Kind::PairingResult, 8000, &result),
        "no PairingResult");
  CHECK(result.pairingResult.success, "pairing refused: %s", result.pairingResult.reason.c_str());

  msg::Envelope state;
  CHECK(headset.WaitForEnvelope(msg::Envelope::Kind::SessionState, 8000, &state),
        "no SessionState");
  CHECK(state.sessionState.input == msg::InputArmState::Armed, "input should be armed");
  CHECK(!state.sessionState.mediaFlowBound, "the flow cannot be bound yet");

  CHECK(!listener.AcceptChannel(25), "a session was accepted before UDP flow binding");

  // ---- the datagram flow is usable only after FlowHello (§4.3) -------------
  CHECK(quic::SendDatagram(client.conn(), FlowHelloBytes(sessionGen + 1)), "send stale FlowHello");
  CHECK(!listener.AcceptChannel(25), "a stale FlowHello activated the session");
  CHECK(quic::SendDatagram(client.conn(), FlowHelloBytes(sessionGen)), "send valid FlowHello");
  msg::Envelope bound;
  CHECK(headset.WaitForEnvelope(msg::Envelope::Kind::SessionState, 8000, &bound),
        "no SessionState after FlowHello");
  CHECK(bound.sessionState.mediaFlowBound, "the media flow stayed unbound");

  std::shared_ptr<Wire1Channel> channel = listener.AcceptChannel(8000);
  CHECK(channel != nullptr, "AcceptChannel timed out");
  if (!channel) return false;
  CHECK(channel->IsConnected(), "the accepted channel is not connected");
  CHECK(channel->peerDeviceId() == "test-headset", "peer device id");
  CHECK(channel->peerName() == "Test Vision Pro", "peer name");
  CHECK(listener.active() == channel, "the listener lost its active session");

  // ---- the pipeline's packed packets become WIRE envelopes -----------------
  const std::vector<uint8_t> layout = PackedLayout();
  CHECK(channel->Send(layout.data(), layout.size()), "send layout");
  msg::Envelope layoutEnv;
  CHECK(headset.WaitForEnvelope(msg::Envelope::Kind::MonitorLayout, 8000, &layoutEnv),
        "no MonitorLayout");
  CHECK(layoutEnv.monitorLayout.layoutGen == 7, "layout generation");
  CHECK(layoutEnv.monitorLayout.monitors.size() == 1, "monitor count");
  if (!layoutEnv.monitorLayout.monitors.empty()) {
    const auto& m = layoutEnv.monitorLayout.monitors[0];
    CHECK(m.monitorId == 1, "the monitor id is the layout index plus one");
    CHECK(m.width == 1920 && m.height == 1080, "size");
    CHECK(m.ppi == 141, "ppi");
    CHECK(m.primary, "primary flag");
  }

  // A codec configuration must wait for the first picture to say which monitor
  // it belongs to: the packed protocol names it only on the media packet.
  const uint64_t codecGen = 3;
  const std::vector<uint8_t> codec = PackedCodecConfig(codecGen);
  CHECK(channel->Send(codec.data(), codec.size()), "send codec config");
  msg::Envelope early;
  CHECK(!headset.WaitForEnvelope(msg::Envelope::Kind::CodecConfig, 300, &early),
        "a codec config went out before a picture named its monitor");

  const uint32_t pictureBytes = 300000;  // several RS blocks, so FEC is exercised
  const uint32_t picSeq = 41;
  const std::vector<uint8_t> media = PackedMedia(picSeq, codecGen, pictureBytes, true);
  CHECK(channel->Send(media.data(), media.size()), "send media");

  msg::Envelope codecEnv;
  CHECK(headset.WaitForEnvelope(msg::Envelope::Kind::CodecConfig, 8000, &codecEnv),
        "the held codec config never went out");
  CHECK(codecEnv.codecConfig.monitorId == 1, "codec config monitor id");
  CHECK(codecEnv.codecConfig.codecGen == codecGen, "codec generation");
  CHECK(codecEnv.codecConfig.codec == msg::Codec::Hevc, "codec");
  CHECK(codecEnv.codecConfig.parameterSets.size() == 2, "parameter sets split from Annex-B: %zu",
        codecEnv.codecConfig.parameterSets.size());
  if (codecEnv.codecConfig.parameterSets.size() == 2) {
    const auto& first = codecEnv.codecConfig.parameterSets[0];
    CHECK(first.size() == 4 && first[0] == 0x40, "the start codes must be gone");
  }

  // ---- the picture as FEC-protected datagrams (§4.1, §5) -------------------
  std::map<uint16_t, std::vector<uint8_t>> block0Data, block0Parity;
  dg::MediaShardHeader sample{};
  bool haveSample = false;
  const ULONGLONG deadline = GetTickCount64() + 8000;
  while (GetTickCount64() < deadline) {
    std::vector<std::vector<uint8_t>> batch;
    headset.DrainDatagrams(kDgMediaShard, 250, &batch);
    for (const auto& d : batch) {
      dg::MediaShardHeader h{};
      if (!dg::MediaShardHeader::parse(d.data(), d.size(), h)) {
        CHECK(false, "malformed shard header");
        continue;
      }
      if (!haveSample) {
        sample = h;
        haveSample = true;
      }
      if (h.blockFirstShard != 0) continue;  // only block 0 is rebuilt below
      const uint8_t* payload = d.data() + dg::MediaShardHeader::kSize;
      const std::vector<uint8_t> body(payload, payload + h.shardBytes);
      if (h.flags & dg::MediaShardHeader::kParity) block0Parity[h.shardIndex - h.k] = body;
      else block0Data[h.shardIndex] = body;
    }
    if (haveSample && block0Data.size() >= size_t(sample.k) && block0Parity.size() >= 2) break;
  }
  CHECK(haveSample, "no media datagrams arrived");
  if (!haveSample) return g_failures == 0;
  CHECK(sample.sessionGen == sessionGen, "shard session generation");
  CHECK(sample.monitorId == 1, "shard monitor id");
  CHECK(sample.picSeq == picSeq, "shard picture sequence");
  CHECK(sample.frameBytes == pictureBytes, "frame bytes in the header");
  CHECK(sample.flags & dg::MediaShardHeader::kRandomAccess, "an IDR is marked random access");
  CHECK(sample.k >= 4, "block 0 has several data shards (k=%u)", sample.k);
  CHECK(sample.m >= 2, "block 0 carries parity (m=%u)", sample.m);
  CHECK(sample.shardBytes == 1052, "shard payload for a 1100 B peer limit (%u)", sample.shardBytes);

  // Throw two data shards away and let Reed-Solomon put them back.
  if (block0Data.size() >= size_t(sample.k) && block0Parity.size() >= 2) {
    const int k = sample.k;
    const size_t shardBytes = sample.shardBytes;
    std::vector<std::vector<uint8_t>> shards(k);
    std::vector<uint8_t> presentFlags(k, 0);
    for (int i = 0; i < k; ++i) {
      auto it = block0Data.find(uint16_t(i));
      if (it != block0Data.end() && i >= 2) {  // shards 0 and 1 are "lost"
        shards[i] = it->second;
        presentFlags[i] = 1;
      } else {
        shards[i].assign(shardBytes, 0xAA);
      }
    }
    std::vector<const uint8_t*> dataPtrs;
    dataPtrs.resize(size_t(k));
    std::vector<const uint8_t*> parityPtrs;
    std::vector<int> parityRows;
    for (auto& kv : block0Parity) {
      parityRows.push_back(int(kv.first));
      parityPtrs.push_back(kv.second.data());
      if (parityPtrs.size() >= 2) break;
    }
    for (int i = 0; i < k; ++i) dataPtrs[i] = shards[i].data();
    // Reconstruct wants a real bool array (sizeof(bool) == 1 on Windows).
    bool present[64] = {};
    for (int i = 0; i < k && i < 64; ++i) present[i] = presentFlags[i] != 0;
    const bool ok = fec::Reconstruct(k, dataPtrs.data(), present, parityRows.data(),
                                     parityPtrs.data(), int(parityPtrs.size()), shardBytes);
    CHECK(ok, "Reed-Solomon could not rebuild the block");
    if (ok) {
      bool same = true;
      for (int i = 0; i < 2; ++i) {
        for (size_t b = 0; b < shardBytes; ++b)
          if (shards[i][b] != AuByte(picSeq, size_t(i) * shardBytes + b)) same = false;
      }
      CHECK(same, "the rebuilt shards do not match the coded picture");
    }
  }

  // ---- a NACK pulls the missing shards straight back (§4.4) ----------------
  dg::Nack nack;
  nack.monitorId = 1;
  nack.codecGen = codecGen;
  nack.picSeq = picSeq;
  nack.missing = {0, 1};
  quic::SendDatagram(client.conn(), dg::Nack::encode(nack));
  bool sawRetransmit = false;
  const ULONGLONG nackDeadline = GetTickCount64() + 4000;
  while (!sawRetransmit && GetTickCount64() < nackDeadline) {
    std::vector<std::vector<uint8_t>> batch;
    if (!headset.DrainDatagrams(kDgMediaShard, 250, &batch)) continue;
    for (const auto& d : batch) {
      dg::MediaShardHeader h{};
      if (!dg::MediaShardHeader::parse(d.data(), d.size(), h)) continue;
      if ((h.flags & dg::MediaShardHeader::kRetransmit) && h.blockFirstShard == 0 &&
          h.shardIndex == 0)
        sawRetransmit = true;
    }
  }
  CHECK(sawRetransmit, "the NACK produced no retransmission");

  // Audio and cursor are sent from the pipeline while the pacer is active.
  // A reconfiguration must also publish a second CodecConfig.
  const auto nextCodec = PackedCodecConfig(codecGen + 1);
  CHECK(channel->SendPacket(nextCodec), "second codec config send");
  const auto nextPicture = PackedMedia(picSeq + 1, codecGen + 1, 4096, true);
  CHECK(channel->SendPacket(nextPicture), "send picture in new codec generation");
  CHECK(headset.WaitForEnvelope(msg::Envelope::Kind::CodecConfig, 1000, &codecEnv) &&
        codecEnv.codecConfig.codecGen == codecGen + 1, "second codec config did not arrive");

  msg::Envelope settings;
  settings.kind = msg::Envelope::Kind::StreamSettings;
  settings.streamSettings.audio = true;
  settings.streamSettings.maxBitrateKbps = 16000;
  headset.SendEnvelope(control, settings);
  auto audioRequest = channel->RecvPacket(1000);
  CHECK(audioRequest.has_value(), "stream settings did not enable pipeline audio");
  CHECK(channel->VideoBitrateLimitKbps() == 16000, "headset bitrate limit was ignored");

  PacketWriter shapePacket(PacketKind::CursorShape);
  CursorShapePayload shape{};
  shape.shapeVer = 1;
  shape.width = shape.height = 1;
  shape.dataSize = 4;
  shapePacket.Append(shape);
  shapePacket.Append(uint32_t(0xffffffff));
  CHECK(channel->SendPacket(shapePacket.Take()), "cursor shape");
  PacketWriter cursorPacket(PacketKind::CursorPos);
  CursorPosPayload cursor{};
  cursor.shapeVer = 1;
  cursor.layoutGen = 7;
  cursor.state = uint8_t(PointerState::Separate);
  cursorPacket.Append(cursor);
  CHECK(channel->SendPacket(cursorPacket.Take()), "cursor position");
  std::vector<std::vector<uint8_t>> cursorDatagrams;
  CHECK(headset.DrainDatagrams(kDgCursor, 1000, &cursorDatagrams), "cursor datagram did not arrive");

  PacketWriter audioPacket(PacketKind::Audio);
  AudioPayload audio{};
  audio.sampleRate = 48000;
  audio.channels = 2;
  audio.frames = 240;
  audio.qpcCapture = QpcNow();
  audioPacket.Append(audio);
  std::vector<int16_t> samples(audio.frames * audio.channels, 42);
  audioPacket.AppendArray(samples);
  CHECK(channel->SendPacket(audioPacket.Take()), "send PCM");
  std::vector<std::vector<uint8_t>> audioDatagrams;
  CHECK(headset.DrainDatagrams(kDgAudio, 1000, &audioDatagrams), "audio datagram did not arrive");
  if (!audioDatagrams.empty())
    CHECK(audioDatagrams.front().size() == dg::AudioHeader::kHeaderSize + samples.size() * 2,
          "PCM audio payload was altered");

  // ---- the ping/pong clock probe (§8) --------------------------------------
  msg::Envelope ping;
  ping.kind = msg::Envelope::Kind::Ping;
  ping.ping.clientSendUs = 987654321ull;
  ping.ping.seq = 5;
  headset.SendEnvelope(control, ping);
  msg::Envelope pong;
  CHECK(headset.WaitForEnvelope(msg::Envelope::Kind::Pong, 8000, &pong), "no Pong");
  CHECK(pong.pong.seq == 5 && pong.pong.clientSendUs == 987654321ull, "the pong must echo the ping");

  // ---- a recovery request becomes the pipeline's own packet ----------------
  msg::Envelope recovery;
  recovery.kind = msg::Envelope::Kind::RecoveryRequest;
  recovery.recoveryRequest.monitorId = 1;
  recovery.recoveryRequest.codecGen = uint32_t(codecGen);
  recovery.recoveryRequest.kind = msg::RecoveryKind::Idr;
  recovery.recoveryRequest.lastGoodPicSeq = picSeq;
  headset.SendEnvelope(control, recovery);
  auto synth = channel->RecvPacket(4000);
  CHECK(synth.has_value(), "no packet reached the pipeline for the recovery request");
  if (synth) {
    auto pv = PacketView::Parse(synth->data(), synth->size());
    CHECK(pv && pv->kind() == PacketKind::RecoveryRequest, "unexpected packet kind");
    if (pv) {
      auto r = pv->Get<RecoveryRequestPayload>();
      CHECK(r && r->kind == 0 && r->codecGen == uint32_t(codecGen), "recovery payload");
    }
  }

  // ---- input arrives on its own stream, after the session generation -------
  QuicStream input = quic::OpenStream(client.conn());
  msg::Envelope openInput;
  openInput.kind = msg::Envelope::Kind::InputStreamOpen;
  openInput.inputStreamOpen.sessionGen = sessionGen;
  headset.SendEnvelope(input, openInput);

  msg::Envelope events;
  events.kind = msg::Envelope::Kind::InputEvents;
  events.input.inputGen = 1;
  msg::InputEvent move;
  move.kind = msg::InputEvent::Kind::Move;
  move.move.monitorId = 1;
  move.move.layoutGen = 7;
  move.move.x = 640.5f;
  move.move.y = 360.25f;
  events.input.events.push_back(move);
  msg::InputEvent button;
  button.kind = msg::InputEvent::Kind::Button;
  button.button.monitorId = 1;
  button.button.layoutGen = 7;
  button.button.x = 640.5f;
  button.button.y = 360.25f;
  button.button.button = msg::MouseButton::Left;
  button.button.down = true;
  events.input.events.push_back(button);
  headset.SendEnvelope(input, events);

  bool sawMove = false, sawButton = false;
  for (int i = 0; i < 8 && !(sawMove && sawButton); ++i) {
    auto packet = channel->RecvPacket(2000);
    if (!packet) break;
    auto pv = PacketView::Parse(packet->data(), packet->size());
    if (!pv || pv->kind() != PacketKind::Input) continue;
    auto hdr = pv->Get<InputPayload>();
    if (!hdr || hdr->count != 2) continue;
    const auto list = pv->ArrayAt<InputEvent>(uint32_t(sizeof(InputPayload)), hdr->count);
    for (const auto& e : list) {
      if (e.kind == uint8_t(InputKind::MoveAbs) && e.x == 640 && e.y == 360 && e.monitor == 0)
        sawMove = true;
      if (e.kind == uint8_t(InputKind::Button) && (e.code & 1) && (e.flags & kKeyUp) == 0)
        sawButton = true;
    }
  }
  CHECK(sawMove, "the pointer move never reached the pipeline");
  CHECK(sawButton, "the pointer button never reached the pipeline");

  // ---- a stale session generation is refused ------------------------------
  QuicStream stale = quic::OpenStream(client.conn());
  msg::Envelope wrongGen;
  wrongGen.kind = msg::Envelope::Kind::InputStreamOpen;
  wrongGen.inputStreamOpen.sessionGen = sessionGen ^ 0x5A5A5A5Au;
  headset.SendEnvelope(stale, wrongGen);
  // The host closes that stream instead of accepting events from an old
  // session; nothing should reach the pipeline as input.
  bool staleLeaked = false;
  for (int i = 0; i < 3; ++i) {
    auto packet = channel->RecvPacket(200);
    if (!packet) break;
    auto pv = PacketView::Parse(packet->data(), packet->size());
    if (pv && pv->kind() == PacketKind::Input) staleLeaked = true;
  }
  CHECK(!staleLeaked, "an input stream from another session was accepted");

  if (measure) MeasureDelivery(*channel, headset, codecGen + 1);

  // ---- disconnect ---------------------------------------------------------
  listener.Stop();
  CHECK(!channel->IsConnected(), "listener stop did not drain the active connection");
  CHECK(headset.WaitClosed(5000), "the headset never saw the connection close");
  client.Close();
  QuicSharedClose();
  return g_failures == 0;
}

}  // namespace

int main(int argc, char** argv) {
  // Unbuffered: a hard crash must not swallow the line that says how far we got.
  setvbuf(stdout, nullptr, _IONBF, 0);
  // A hard CRT failure (a null %s, a bad memcpy) is a silent fast fail without
  // this; the two handlers below turn it into a line in the test log.
  _set_invalid_parameter_handler([](const wchar_t* expr, const wchar_t* fn, const wchar_t* file,
                                    unsigned line, uintptr_t) {
    std::printf("INVALID PARAMETER: %S in %S at %S:%u\n", expr ? expr : L"?",
                fn ? fn : L"?", file ? file : L"?", line);
    fflush(stdout);
  });
  // An exception that reaches terminate() would otherwise die silently, and a
  // test that dies silently teaches nothing.
  std::set_terminate([] {
    auto e = std::current_exception();
    if (!e) {
      std::printf("TERMINATE: no exception (a no-throw violation?)\n");
    } else {
      try {
        std::rethrow_exception(e);
      } catch (const std::exception& ex) {
        std::printf("TERMINATE: %s\n", ex.what());
      } catch (...) {
        std::printf("TERMINATE: an exception not derived from std::exception\n");
      }
    }
    fflush(stdout);
    abort();
  });
  log::Init("quic-test", log::Level::Info);
  const bool ok = Run(argc > 1 && std::strcmp(argv[1], "--measure") == 0);
  std::printf(ok ? "quic session test: OK\n" : "quic session test: %d FAILURE(S)\n", g_failures);
  return ok ? 0 : 1;
}

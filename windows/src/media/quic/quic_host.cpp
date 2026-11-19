#include "quic_host.h"

#include <ws2tcpip.h>

#include <memory>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <set>

#define QUIC_API_ENABLE_PREVIEW_FEATURES 1  // ConnectionExportKeyingMaterial (WIRE.md §7)
#include <msquic.h>

#include "identity.h"
#include "wdauth.h"
#include "wvd/logging.h"

namespace wvd {

struct QuicServerState {
  IQuicSink* sink = nullptr;
  HQUIC config = nullptr;
  std::mutex mutex;
  std::condition_variable drained;
  std::set<HQUIC> connections;
};

namespace {

const QUIC_API_TABLE* Api() {
  static const QUIC_API_TABLE* api = []() -> const QUIC_API_TABLE* {
    const QUIC_API_TABLE* a = nullptr;
    if (QUIC_FAILED(MsQuicOpenVersion(QUIC_API_VERSION_2, (const void**)&a))) return nullptr;
    return a;
  }();
  return api;
}

std::atomic<int> gUsers{0};

// One registration per process: it owns msquic's worker threads, which are far
// more expensive to create than to keep.
HQUIC Registration() {
  static HQUIC reg = []() -> HQUIC {
    QUIC_REGISTRATION_CONFIG cfg{"wvmhost", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
    HQUIC r = nullptr;
    if (QUIC_FAILED(Api()->RegistrationOpen(&cfg, &r))) return nullptr;
    return r;
  }();
  return reg;
}

// ---- per-stream send queue -------------------------------------------------

// MsQuic orders the stream writes. Keep their data alive until completion,
// and bound both unacknowledged and queued reliable control bytes.
struct StreamSendBlock {
  std::vector<uint8_t> bytes;
  QUIC_BUFFER buffer;
};

struct StreamCtx {
  HQUIC conn = nullptr;
  IQuicSink* sink = nullptr;
  std::atomic<size_t> pendingBytes{0};
};

constexpr size_t kStreamQueueLimit = 4u << 20;

QUIC_STATUS _stdcall StreamCb(HQUIC stream, void* ctx, QUIC_STREAM_EVENT* ev) {
  auto* sc = static_cast<StreamCtx*>(ctx);
  switch (ev->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
      for (uint32_t i = 0; i < ev->RECEIVE.BufferCount; ++i) {
        const QUIC_BUFFER& b = ev->RECEIVE.Buffers[i];
        if (b.Length) sc->sink->OnQuicStreamData(sc->conn, stream, b.Buffer, b.Length);
      }
      // Consumed synchronously: returning SUCCESS *is* the completion. Calling
      // StreamReceiveComplete here would be wrong — that call belongs to a
      // callback that returned QUIC_STATUS_PENDING — and it stalls the stream
      // after the first receive.
      return QUIC_STATUS_SUCCESS;
    }
    case QUIC_STREAM_EVENT_SEND_COMPLETE: {
      auto* block = static_cast<StreamSendBlock*>(ev->SEND_COMPLETE.ClientContext);
      sc->pendingBytes.fetch_sub(block->bytes.size(), std::memory_order_relaxed);
      delete block;
      return QUIC_STATUS_SUCCESS;
    }
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
      sc->sink->OnQuicStreamClosed(sc->conn, stream);
      Api()->StreamClose(stream);
      delete sc;
      return QUIC_STATUS_SUCCESS;
    default:
      return QUIC_STATUS_SUCCESS;
  }
}

// ---- per-connection state --------------------------------------------------

struct ConnCtx {
  HQUIC conn = nullptr;
  IQuicSink* sink = nullptr;
  QuicServerState* server = nullptr;
  std::atomic<uint32_t> datagramLimit{0};
  std::atomic<size_t> datagramPendingBytes{0};
  std::atomic<uint64_t> datagramsSent{0}, datagramsDropped{0};
  std::vector<uint8_t> pinCertSha256;  // client: refuse any other leaf
  std::vector<uint8_t> peerCertHash;
  uint32_t lastStatus = 0;  // transport shutdown code, for the sink
  // The client keeps its handle until the sink's final callback returns.
  std::mutex m;
  std::condition_variable finished;
  bool done = false;
};

// The descriptor and bytes live until MsQuic reports sent or canceled.
struct DatagramBlock {
  std::vector<uint8_t> bytes;
  QUIC_BUFFER qb;
};

ConnCtx* ConnOf(HQUIC conn) { return static_cast<ConnCtx*>(Api()->GetContext(conn)); }

StreamCtx* StreamOf(HQUIC stream) { return static_cast<StreamCtx*>(Api()->GetContext(stream)); }

QUIC_STATUS _stdcall ConnCb(HQUIC conn, void* ctx, QUIC_CONNECTION_EVENT* ev) {
  auto* cc = static_cast<ConnCtx*>(ctx);
  switch (ev->Type) {
    case QUIC_CONNECTION_EVENT_PEER_CERTIFICATE_RECEIVED: {
      // The client's only authentication is the pinned leaf hash (WIRE.md §7);
      // the certificate itself is self-signed and reaches us unvalidated.
      auto* cert = reinterpret_cast<PCCERT_CONTEXT>(ev->PEER_CERTIFICATE_RECEIVED.Certificate);
      if (!cert || !cert->pbCertEncoded) return QUIC_STATUS_INTERNAL_ERROR;
      cc->peerCertHash = Sha256(cert->pbCertEncoded, cert->cbCertEncoded);
      if (!cc->pinCertSha256.empty() && cc->peerCertHash != cc->pinCertSha256) {
        WVD_LOGW("quic: host certificate does not match the pinned hash — handshake refused");
        return QUIC_STATUS_INTERNAL_ERROR;
      }
      return QUIC_STATUS_SUCCESS;
    }
    case QUIC_CONNECTION_EVENT_CONNECTED:
      try {
        cc->sink->OnQuicConnected(conn, cc->peerCertHash);
      } catch (const std::exception& e) {
        WVD_LOGE("quic: the connect callback threw %s", e.what());
      } catch (...) {
        WVD_LOGE("quic: the connect callback threw a non-standard exception");
      }
      return QUIC_STATUS_SUCCESS;
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
      HQUIC s = ev->PEER_STREAM_STARTED.Stream;
      auto* sc = new StreamCtx();
      sc->conn = conn;
      sc->sink = cc->sink;
      Api()->SetCallbackHandler(s, reinterpret_cast<void*>(StreamCb), sc);
      Api()->SetContext(s, sc);
      cc->sink->OnQuicStream(conn, s);
      return QUIC_STATUS_SUCCESS;
    }
    case QUIC_CONNECTION_EVENT_DATAGRAM_STATE_CHANGED:
      cc->datagramLimit.store(ev->DATAGRAM_STATE_CHANGED.SendEnabled ?
                                  ev->DATAGRAM_STATE_CHANGED.MaxSendLength : 0,
                              std::memory_order_relaxed);
      return QUIC_STATUS_SUCCESS;
    case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED:
      if (ev->DATAGRAM_RECEIVED.Buffer && ev->DATAGRAM_RECEIVED.Buffer->Length)
        cc->sink->OnQuicDatagram(conn, ev->DATAGRAM_RECEIVED.Buffer->Buffer,
                                 ev->DATAGRAM_RECEIVED.Buffer->Length);
      return QUIC_STATUS_SUCCESS;
    case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED: {
      auto& report = ev->DATAGRAM_SEND_STATE_CHANGED;
      auto* block = static_cast<DatagramBlock*>(report.ClientContext);
      if (block && (report.State == QUIC_DATAGRAM_SEND_SENT ||
                    QUIC_DATAGRAM_SEND_STATE_IS_FINAL(report.State))) {
        cc->datagramPendingBytes.fetch_sub(block->bytes.size(), std::memory_order_relaxed);
        auto& count = report.State == QUIC_DATAGRAM_SEND_SENT ? cc->datagramsSent : cc->datagramsDropped;
        count.fetch_add(1, std::memory_order_relaxed);
        delete block;
        report.ClientContext = nullptr;
      }
      return QUIC_STATUS_SUCCESS;
    }
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
      cc->lastStatus = static_cast<uint32_t>(ev->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
      WVD_LOGV("quic: transport shut the connection down (0x%x)",
               ev->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
      return QUIC_STATUS_SUCCESS;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
      return QUIC_STATUS_SUCCESS;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: {
      // The sink is still owned by whoever created it, and that thread may be
      // waiting here to know it is safe to destroy it: report first, retire
      // after.
      cc->sink->OnQuicClosed(conn, cc->lastStatus);
      if (auto* server = cc->server) {
        {
          std::lock_guard<std::mutex> lock(server->mutex);
          server->connections.erase(conn);
          server->drained.notify_all();
        }
        Api()->ConnectionClose(conn);
        delete cc;
        return QUIC_STATUS_SUCCESS;
      }
      {
        std::lock_guard<std::mutex> lock(cc->m);
        cc->done = true;
        cc->finished.notify_one();
      }
      return QUIC_STATUS_SUCCESS;
    }
    default:
      return QUIC_STATUS_SUCCESS;
  }
}

// ---- listener --------------------------------------------------------------

QUIC_STATUS _stdcall ListenerCb(HQUIC, void* ctx, QUIC_LISTENER_EVENT* ev) {
  if (ev->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION) return QUIC_STATUS_SUCCESS;
  auto* server = static_cast<QuicServerState*>(ctx);
  auto cc = std::make_unique<ConnCtx>();
  cc->conn = ev->NEW_CONNECTION.Connection;
  cc->sink = server->sink;
  cc->server = server;
  const HQUIC conn = cc->conn;
  {
    std::lock_guard<std::mutex> lock(server->mutex);
    server->connections.insert(conn);
  }
  Api()->SetCallbackHandler(conn, reinterpret_cast<void*>(ConnCb), cc.get());
  const QUIC_STATUS status = Api()->ConnectionSetConfiguration(conn, server->config);
  if (QUIC_SUCCEEDED(status)) {
    cc.release();
  } else {
    std::lock_guard<std::mutex> lock(server->mutex);
    server->connections.erase(conn);
    WVD_LOGW("quic: ConnectionSetConfiguration failed 0x%x", status);
  }
  return status;
}

// Shared settings: datagrams on (RFC 9221), 15 s idle (WIRE.md §2).
void ApplySettings(QUIC_SETTINGS& settings, uint32_t idleMs, uint32_t handshakeMs,
                   uint16_t peerBidiStreams) {
  memset(&settings, 0, sizeof(settings));
  settings.IdleTimeoutMs = idleMs;
  settings.IsSet.IdleTimeoutMs = TRUE;
  settings.HandshakeIdleTimeoutMs = handshakeMs;
  settings.IsSet.HandshakeIdleTimeoutMs = TRUE;
  settings.DatagramReceiveEnabled = TRUE;
  settings.IsSet.DatagramReceiveEnabled = TRUE;
  settings.PeerBidiStreamCount = peerBidiStreams;
  settings.IsSet.PeerBidiStreamCount = TRUE;
  // The reliable control queue owns and bounds its send buffers.
  settings.SendBufferingEnabled = FALSE;
  settings.IsSet.SendBufferingEnabled = TRUE;
}

}  // namespace

// ---- engine ----------------------------------------------------------------

bool QuicSharedOpen(std::string* error) {
  static std::mutex m;
  std::lock_guard<std::mutex> lk(m);
  if (!Api() || !Registration()) {
    if (error) *error = "quic: msquic could not start (is msquic.dll next to the exe?)";
    return false;
  }
  ++gUsers;
  return true;
}

void QuicSharedClose() {
  static std::mutex m;
  std::lock_guard<std::mutex> lk(m);
  if (gUsers > 0) --gUsers;
  // The API table and the registration stay open for the process: unloading
  // msquic while one of its worker threads is inside it is a shutdown race
  // that no part of the product needs.
}

void* QuicRegistration() { return Registration(); }

// ---- server ----------------------------------------------------------------

QuicServer::QuicServer() = default;

QuicServer::~QuicServer() { Stop(); }

bool QuicServer::Start(const QuicServerOptions& options, HostIdentity& identity, IQuicSink& sink,
                       std::string* error) {
  Stop();
  if (!QuicSharedOpen(error)) return false;
  const QUIC_API_TABLE* api = Api();

  auto state = std::make_unique<QuicServerState>();
  state->sink = &sink;

  QUIC_SETTINGS settings;
  ApplySettings(settings, options.idleTimeoutMs, options.handshakeIdleTimeoutMs,
                options.peerBidiStreamCount);
  QUIC_BUFFER alpn{static_cast<UINT32>(strlen(options.alpn)),
                   reinterpret_cast<uint8_t*>(const_cast<char*>(options.alpn))};
  HQUIC config = nullptr;
  if (QUIC_FAILED(api->ConfigurationOpen(static_cast<HQUIC>(QuicRegistration()), &alpn, 1,
                                         &settings, sizeof(settings), nullptr, &config))) {
    if (error) *error = "quic: ConfigurationOpen failed";
    return false;
  }
  state->config = config;

  QUIC_CREDENTIAL_CONFIG cred{};
  cred.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_CONTEXT;
  cred.CertificateContext =
      reinterpret_cast<QUIC_CERTIFICATE*>(const_cast<CERT_CONTEXT*>(identity.certContext()));
  cred.Flags = QUIC_CREDENTIAL_FLAG_NONE;
  if (QUIC_FAILED(api->ConfigurationLoadCredential(config, &cred))) {
    if (error) *error = "quic: the host certificate could not be loaded into the TLS stack";
    api->ConfigurationClose(config);
    return false;
  }

  HQUIC listener = nullptr;
  if (QUIC_FAILED(api->ListenerOpen(static_cast<HQUIC>(QuicRegistration()), ListenerCb, state.get(),
                                    &listener))) {
    if (error) *error = "quic: ListenerOpen failed";
    api->ConfigurationClose(config);
    return false;
  }
  state_ = state.release();
  listener_ = listener;

  SOCKADDR_INET addr{};
  addr.si_family = AF_INET;
  addr.Ipv4.sin_family = AF_INET;
  addr.Ipv4.sin_port = htons(options.port);
  addr.Ipv4.sin_addr.S_un.S_addr =
      options.loopbackOnly ? htonl(INADDR_LOOPBACK) : INADDR_ANY;
  const QUIC_STATUS st = api->ListenerStart(listener, &alpn, 1,
                                            reinterpret_cast<QUIC_ADDR*>(&addr));
  if (QUIC_FAILED(st)) {
    if (error) {
      *error = std::string("quic: UDP listen on port ") + std::to_string(options.port) +
               " failed (0x" + std::to_string(static_cast<unsigned>(st)) +
               ") — check for another host on this port and the Windows Firewall";
    }
    Stop();
    return false;
  }
  // With port 0 the OS picked one; report what is actually bound (tests).
  port_ = options.port;
  if (port_ == 0) {
    SOCKADDR_INET local{};
    uint32_t len = sizeof(local);
    if (QUIC_SUCCEEDED(api->GetParam(listener, QUIC_PARAM_LISTENER_LOCAL_ADDRESS, &len,
                                     &local)))
      port_ = ntohs(local.Ipv4.sin_port);
  }
  WVD_LOGI("quic: listening on udp:%u (ALPN %s, idle %u ms)", port_, options.alpn,
           options.idleTimeoutMs);
  return true;
}

void QuicServer::Stop() {
  if (!state_) return;
  const auto* api = Api();
  api->ListenerClose(static_cast<HQUIC>(listener_));
  {
    std::unique_lock<std::mutex> lock(state_->mutex);
    for (HQUIC conn : state_->connections)
      api->ConnectionShutdown(conn, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
    state_->drained.wait(lock, [this] { return state_->connections.empty(); });
  }
  api->ConfigurationClose(state_->config);
  delete state_;
  state_ = nullptr;
  listener_ = nullptr;
  port_ = 0;
}

// ---- client ----------------------------------------------------------------

QuicClient::QuicClient() = default;

QuicClient::~QuicClient() { Close(); }

bool QuicClient::Connect(const std::string& host, uint16_t port, IQuicSink& sink,
                         std::string* error, const std::vector<uint8_t>& verifyPeerCertSha256,
                         uint32_t idleTimeoutMs) {
  if (!QuicSharedOpen(error)) return false;
  const QUIC_API_TABLE* api = Api();

  QUIC_SETTINGS settings;
  ApplySettings(settings, idleTimeoutMs, 10000, 8);
  QUIC_BUFFER alpn{static_cast<UINT32>(strlen(kQuicAlpn)),
                   reinterpret_cast<uint8_t*>(const_cast<char*>(kQuicAlpn))};
  HQUIC config = nullptr;
  if (QUIC_FAILED(api->ConfigurationOpen(static_cast<HQUIC>(QuicRegistration()), &alpn, 1,
                                         &settings, sizeof(settings), nullptr, &config))) {
    if (error) *error = "quic client: ConfigurationOpen failed";
    return false;
  }
  config_ = config;
  QUIC_CREDENTIAL_CONFIG cred{};
  cred.Type = QUIC_CREDENTIAL_TYPE_NONE;
  // The certificate is self-signed: we validate it ourselves by hash (§7).
  cred.Flags = QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION |
               QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED;
  if (QUIC_FAILED(api->ConfigurationLoadCredential(config, &cred))) {
    if (error) *error = "quic client: ConfigurationLoadCredential failed";
    return false;
  }

  auto* cc = new ConnCtx();
  cc->sink = &sink;
  cc->pinCertSha256 = verifyPeerCertSha256;
  if (QUIC_FAILED(api->ConnectionOpen(static_cast<HQUIC>(QuicRegistration()), ConnCb, cc,
                                      &cc->conn))) {
    if (error) *error = "quic client: ConnectionOpen failed";
    delete cc;
    return false;
  }
  Api()->SetContext(cc->conn, cc);
  if (QUIC_FAILED(api->ConnectionStart(cc->conn, static_cast<HQUIC>(config_), AF_INET,
                                       host.c_str(), port))) {
    if (error) *error = "quic client: ConnectionStart failed";
    api->ConnectionClose(cc->conn);
    delete cc;
    return false;
  }
  conn_ = cc->conn;
  return true;
}

void QuicClient::Close() {
  const QUIC_API_TABLE* api = Api();
  if (conn_) {
    const auto connection = static_cast<HQUIC>(conn_);
    auto* cc = ConnOf(connection);
    api->ConnectionShutdown(connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
    {
      std::unique_lock<std::mutex> lock(cc->m);
      cc->finished.wait(lock, [cc] { return cc->done; });
    }
    api->ConnectionClose(connection);
    delete cc;
    conn_ = nullptr;
  }
  if (config_) {
    api->ConfigurationClose(static_cast<HQUIC>(config_));
    config_ = nullptr;
  }
}

// ---- per-connection operations --------------------------------------------

namespace quic {

bool ExportKeyingMaterial(QuicConn conn, const char* label, std::vector<uint8_t>& out) {
  // msquic writes Config->OutputLength bytes; leaving it zero asks for nothing
  // and fails the call with E_INVALIDARG.
  constexpr uint32_t kExportLength = 32;
  QUIC_KEYING_MATERIAL_CONFIG cfg{};
  cfg.Label = label;
  cfg.ContextLength = 0;
  cfg.Context = nullptr;
  cfg.OutputLength = kExportLength;
  out.assign(kExportLength, 0);
  const QUIC_STATUS st = Api()->ConnectionExportKeyingMaterial(
      static_cast<HQUIC>(conn), &cfg, out.data());
  if (QUIC_FAILED(st)) {
    out.clear();
    WVD_LOGW("quic: exporter '%s' failed 0x%x", label, st);
    return false;
  }
  return true;
}

uint32_t MaxDatagramPayload(QuicConn conn) {
  auto* cc = conn ? ConnOf(static_cast<HQUIC>(conn)) : nullptr;
  return cc ? cc->datagramLimit.load(std::memory_order_relaxed) : 0;
}

DatagramStats GetDatagramStats(QuicConn conn) {
  auto* cc = conn ? ConnOf(static_cast<HQUIC>(conn)) : nullptr;
  if (!cc) return {};
  return {cc->datagramPendingBytes.load(std::memory_order_relaxed),
          cc->datagramsSent.load(std::memory_order_relaxed),
          cc->datagramsDropped.load(std::memory_order_relaxed)};
}

std::string PeerAddress(QuicConn conn) {
  SOCKADDR_INET addr{};
  uint32_t len = sizeof(addr);
  if (QUIC_FAILED(Api()->GetParam(static_cast<HQUIC>(conn), QUIC_PARAM_CONN_REMOTE_ADDRESS,
                                  &len, &addr)))
    return {};
  char text[INET6_ADDRSTRLEN] = {};
  const void* raw = addr.si_family == AF_INET6 ? static_cast<const void*>(&addr.Ipv6.sin6_addr)
                                               : static_cast<const void*>(&addr.Ipv4.sin_addr);
  if (!inet_ntop(addr.si_family == AF_INET6 ? AF_INET6 : AF_INET, const_cast<void*>(raw), text,
                 sizeof(text)))
    return {};
  const uint16_t port = ntohs(addr.si_family == AF_INET6 ? addr.Ipv6.sin6_port
                                                         : addr.Ipv4.sin_port);
  return std::string(text) + ":" + std::to_string(port);
}

bool SendStream(QuicStream stream, const uint8_t* data, size_t len) {
  auto* sc = stream ? StreamOf(static_cast<HQUIC>(stream)) : nullptr;
  if (!sc || !len || len > kStreamQueueLimit) return false;
  size_t pending = sc->pendingBytes.load(std::memory_order_relaxed);
  do {
    if (pending + len > kStreamQueueLimit) return false;
  } while (!sc->pendingBytes.compare_exchange_weak(pending, pending + len,
                                                  std::memory_order_relaxed));

  auto* block = new StreamSendBlock{{data, data + len}, {}};
  block->buffer = {static_cast<uint32_t>(len), block->bytes.data()};
  const auto status = Api()->StreamSend(static_cast<HQUIC>(stream), &block->buffer, 1,
                                         QUIC_SEND_FLAG_NONE, block);
  if (QUIC_SUCCEEDED(status)) return true;
  sc->pendingBytes.fetch_sub(len, std::memory_order_relaxed);
  delete block;
  return false;
}

bool SendDatagram(QuicConn conn, std::vector<uint8_t> data, bool urgent) {
  auto* cc = conn ? ConnOf(static_cast<HQUIC>(conn)) : nullptr;
  if (!cc || data.empty()) return false;
  const size_t bytes = data.size();
  // Bound data waiting behind MsQuic's congestion window. Leave room for
  // cursor/audio and recovery even when video is backlogged.
  constexpr size_t kVideoQueueLimit = 64 * 1024;
  constexpr size_t kUrgentReserve = 8 * 1024;
  const size_t limit = kVideoQueueLimit + (urgent ? kUrgentReserve : 0);
  if (bytes > cc->datagramLimit.load(std::memory_order_relaxed)) {
    cc->datagramsDropped.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  size_t pending = cc->datagramPendingBytes.load(std::memory_order_relaxed);
  do {
    if (pending + bytes > limit) {
      cc->datagramsDropped.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
  } while (!cc->datagramPendingBytes.compare_exchange_weak(
      pending, pending + bytes, std::memory_order_relaxed));

  auto* block = new DatagramBlock{std::move(data), {}};
  block->qb = {static_cast<uint32_t>(bytes), block->bytes.data()};
  const auto flags = urgent ? QUIC_SEND_FLAG_DGRAM_PRIORITY : QUIC_SEND_FLAG_NONE;
  const QUIC_STATUS status = Api()->DatagramSend(static_cast<HQUIC>(conn), &block->qb, 1,
                                                flags, block);
  if (QUIC_SUCCEEDED(status)) return true;
  delete block;
  cc->datagramPendingBytes.fetch_sub(bytes, std::memory_order_relaxed);
  cc->datagramsDropped.fetch_add(1, std::memory_order_relaxed);
  return false;
}

QuicStream OpenStream(QuicConn conn) {
  ConnCtx* cc = conn ? ConnOf(static_cast<HQUIC>(conn)) : nullptr;
  if (!cc) return nullptr;
  auto* sc = new StreamCtx();
  sc->conn = static_cast<HQUIC>(conn);
  sc->sink = cc->sink;
  HQUIC stream = nullptr;
  if (QUIC_FAILED(Api()->StreamOpen(sc->conn, QUIC_STREAM_OPEN_FLAG_NONE, StreamCb, sc,
                                    &stream))) {
    delete sc;
    return nullptr;
  }
  Api()->SetContext(stream, sc);
  if (QUIC_FAILED(Api()->StreamStart(stream, QUIC_STREAM_START_FLAG_NONE))) {
    Api()->StreamClose(stream);
    // StreamStart failure invalidates nothing else; the ctx is ours to free.
    delete sc;
    return nullptr;
  }
  return stream;
}

void CloseConnection(QuicConn conn, uint32_t appErrorCode) {
  if (!conn) return;
  Api()->ConnectionShutdown(static_cast<HQUIC>(conn), QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                            appErrorCode);
}

void CloseStream(QuicStream stream, uint32_t appErrorCode) {
  if (!stream) return;
  Api()->StreamShutdown(static_cast<HQUIC>(stream),
                        QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL | QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE,
                        appErrorCode);
}

}  // namespace quic
}  // namespace wvd

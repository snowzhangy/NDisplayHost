#include "quic_host.h"

#include <ws2tcpip.h>

#include <memory>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>

#define QUIC_API_ENABLE_PREVIEW_FEATURES 1  // ConnectionExportKeyingMaterial (WIRE.md §7)
#include <msquic.h>

#include "identity.h"
#include "wdauth.h"
#include "wvd/logging.h"

namespace wvd {
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

// msquic keeps reading a send buffer until it reports SEND_COMPLETE, so the
// bytes in flight live here and only here.
struct StreamCtx {
  HQUIC conn = nullptr;
  HQUIC stream = nullptr;
  IQuicSink* sink = nullptr;
  std::mutex m;
  std::deque<std::vector<uint8_t>> out;
  // The block handed to msquic. Both the bytes and the QUIC_BUFFER that
  // describes them must outlive the StreamSend call � msquic reads both until
  // SEND_COMPLETE � so they live in one heap object owned here meanwhile.
  struct InFlight {
    std::vector<uint8_t> bytes;
    QUIC_BUFFER qb;
  };
  InFlight* inFlight = nullptr;
  size_t queued = 0;
  bool pending = false;  // a StreamSend is outstanding
  bool closed = false;
};

// Largest single msquic stream send (QUIC_BUFFER length is 16-bit).
constexpr size_t kStreamChunk = 60u * 1024u;
// A peer that lets this much control data pile up has stopped reading.
constexpr size_t kStreamQueueLimit = 4u << 20;

// Called without the lock; picks up the next queued block if nothing is in
// flight. Safe against a completion delivered on this same thread.
void StreamPump(StreamCtx* sc) {
  StreamCtx::InFlight* send = nullptr;
  {
    std::lock_guard<std::mutex> lk(sc->m);
    if (sc->closed || sc->pending || sc->out.empty()) return;
    send = new StreamCtx::InFlight();
    send->bytes = std::move(sc->out.front());
    sc->out.pop_front();
    sc->queued -= send->bytes.size();
    send->qb.Length = static_cast<UINT32>(send->bytes.size());
    send->qb.Buffer = send->bytes.data();
    sc->pending = true;
    sc->inFlight = send;
  }
  const QUIC_STATUS st = Api()->StreamSend(sc->stream, &send->qb, 1, QUIC_SEND_FLAG_NONE, send);
  if (QUIC_FAILED(st)) {
    WVD_LOGW("quic: StreamSend failed 0x%x", st);
    std::lock_guard<std::mutex> lk(sc->m);
    // No completion is coming for a call that failed outright.
    if (sc->inFlight == send) {
      sc->inFlight = nullptr;
      sc->pending = false;
      delete send;
    }
    sc->closed = true;
  }
}

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
      auto* buf = static_cast<StreamCtx::InFlight*>(ev->SEND_COMPLETE.ClientContext);
      {
        std::lock_guard<std::mutex> lk(sc->m);
        // Only this callback may release the buffer; a shutdown that already
        // reclaimed it leaves the pointer null.
        if (sc->inFlight == buf) {
          sc->inFlight = nullptr;
          sc->pending = false;
        } else {
          buf = nullptr;
        }
        if (ev->SEND_COMPLETE.Canceled) sc->closed = true;
      }
      delete buf;
      StreamPump(sc);
      return QUIC_STATUS_SUCCESS;
    }
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
      bool wasClosed = false;
      StreamCtx::InFlight* stranded = nullptr;
      {
        std::lock_guard<std::mutex> lk(sc->m);
        wasClosed = sc->closed;
        sc->closed = true;
        // A send that never completed dies with the stream.
        stranded = sc->inFlight;
        sc->inFlight = nullptr;
      }
      delete stranded;
      if (!wasClosed) sc->sink->OnQuicStreamClosed(sc->conn, stream);
      Api()->StreamClose(stream);
      delete sc;
      return QUIC_STATUS_SUCCESS;
    }
    default:
      return QUIC_STATUS_SUCCESS;
  }
}

// ---- per-connection state --------------------------------------------------

using FinishedFlag = std::shared_ptr<std::atomic<bool>>;

struct ConnCtx {
  HQUIC conn = nullptr;
  IQuicSink* sink = nullptr;
  bool server = false;
  std::vector<uint8_t> pinCertSha256;  // client: refuse any other leaf
  std::vector<uint8_t> peerCertHash;
  FinishedFlag finished;
  uint32_t lastStatus = 0;  // transport shutdown code, for the sink
  // A msquic connection handle is closed exactly once, by whichever side ends
  // last: the callback when the connection is gone, or the owner (a client) if
  // it has already stopped caring. Connections the listener accepted have no
  // owner, so their side of that bargain starts closed.
  std::mutex m;
  bool done = false;       // SHUTDOWN_COMPLETE has been handled
  bool ownerGone = false;  // the owner released it (always true for servers)
};

// One datagram in flight. msquic keeps the QUIC_BUFFER array *and* the payload
// until the send state becomes final, so the descriptor lives beside the bytes
// and the state-change event frees both; a descriptor on the stack is dead
// memory by the time the flush actually reads it.
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
      sc->stream = s;
      sc->sink = cc->sink;
      Api()->SetCallbackHandler(s, reinterpret_cast<void*>(StreamCb), sc);
      Api()->SetContext(s, sc);
      cc->sink->OnQuicStream(conn, s);
      return QUIC_STATUS_SUCCESS;
    }
    case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED:
      if (ev->DATAGRAM_RECEIVED.Buffer && ev->DATAGRAM_RECEIVED.Buffer->Length)
        cc->sink->OnQuicDatagram(conn, ev->DATAGRAM_RECEIVED.Buffer->Buffer,
                                 ev->DATAGRAM_RECEIVED.Buffer->Length);
      return QUIC_STATUS_SUCCESS;
    case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED:
      // Our send block is ours until the state is final.
      if (QUIC_DATAGRAM_SEND_STATE_IS_FINAL(ev->DATAGRAM_SEND_STATE_CHANGED.State))
        delete static_cast<std::vector<uint8_t>*>(ev->DATAGRAM_SEND_STATE_CHANGED.ClientContext);
      return QUIC_STATUS_SUCCESS;
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
      bool retire = false;
      {
        std::lock_guard<std::mutex> lk(cc->m);
        cc->done = true;
        retire = cc->ownerGone;  // the owner is gone: the handle and ctx are ours
      }
      if (cc->finished) cc->finished->store(true);
      if (retire) {
        Api()->ConnectionClose(conn);
        delete cc;
      }
      return QUIC_STATUS_SUCCESS;
    }
    default:
      return QUIC_STATUS_SUCCESS;
  }
}

// ---- listener --------------------------------------------------------------

struct ServerCtx {
  IQuicSink* sink = nullptr;
  HQUIC config = nullptr;
};

QUIC_STATUS _stdcall ListenerCb(HQUIC listener, void* ctx, QUIC_LISTENER_EVENT* ev) {
  if (ev->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION) return QUIC_STATUS_NOT_SUPPORTED;
  auto* s = static_cast<ServerCtx*>(ctx);
  auto* cc = new ConnCtx();
  cc->conn = ev->NEW_CONNECTION.Connection;
  cc->sink = s->sink;
  cc->server = true;
  cc->ownerGone = true;  // nothing outlives the callback on this side
  Api()->SetCallbackHandler(cc->conn, reinterpret_cast<void*>(ConnCb), cc);
  Api()->SetContext(cc->conn, cc);
  const QUIC_STATUS st = Api()->ConnectionSetConfiguration(cc->conn, s->config);
  if (QUIC_FAILED(st)) {
    WVD_LOGW("quic: ConnectionSetConfiguration failed 0x%x", st);
    Api()->ConnectionClose(cc->conn);
    delete cc;
    return st;
  }
  return QUIC_STATUS_SUCCESS;
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
  // Media is throwaway: never let a stalled connection buffer more than a
  // second of work, and keep the datagram path the priority.
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

  static ServerCtx sctx;  // one listener per process is enough for a host
  sctx.sink = &sink;

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
  config_ = config;
  sctx.config = config;

  QUIC_CREDENTIAL_CONFIG cred{};
  cred.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_CONTEXT;
  cred.CertificateContext =
      reinterpret_cast<QUIC_CERTIFICATE*>(const_cast<CERT_CONTEXT*>(identity.certContext()));
  cred.Flags = QUIC_CREDENTIAL_FLAG_NONE;
  if (QUIC_FAILED(api->ConfigurationLoadCredential(config, &cred))) {
    if (error) *error = "quic: the host certificate could not be loaded into the TLS stack";
    api->ConfigurationClose(config);
    config_ = nullptr;
    sctx.config = nullptr;
    return false;
  }

  HQUIC listener = nullptr;
  if (QUIC_FAILED(api->ListenerOpen(static_cast<HQUIC>(QuicRegistration()), ListenerCb, &sctx,
                                    &listener))) {
    if (error) *error = "quic: ListenerOpen failed";
    Stop();
    return false;
  }
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
    SOCKADDR_INET addr{};
    uint32_t len = sizeof(addr);
    if (QUIC_SUCCEEDED(api->GetParam(listener, QUIC_PARAM_LISTENER_LOCAL_ADDRESS, &len,
                                     &addr)))
      port_ = ntohs(addr.Ipv6.sin6_port);
  }
  WVD_LOGI("quic: listening on udp:%u (ALPN %s, idle %u ms)", port_, options.alpn,
           options.idleTimeoutMs);
  return true;
}

void QuicServer::Stop() {
  const QUIC_API_TABLE* api = Api();
  if (!api) return;
  if (listener_) api->ListenerClose(static_cast<HQUIC>(listener_));
  listener_ = nullptr;
  if (config_) api->ConfigurationClose(static_cast<HQUIC>(config_));
  config_ = nullptr;
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
  cc->server = false;
  cc->pinCertSha256 = verifyPeerCertSha256;
  cc->finished = std::make_shared<std::atomic<bool>>(false);
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
    ConnCtx* cc = ConnOf(static_cast<HQUIC>(conn_));
    FinishedFlag flag;
    bool retire = false;
    if (cc) {
      // Declaring that we are done with the connection must be atomic with the
      // shutdown call: the callback may retire it at any moment afterwards, and
      // a handle that was closed under us must never be touched again.
      std::lock_guard<std::mutex> lk(cc->m);
      cc->ownerGone = true;
      flag = cc->finished;
      if (cc->done) retire = true;  // the callback finished first: the handle is ours
      else api->ConnectionShutdown(static_cast<HQUIC>(conn_), QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
    }
    if (retire) {
      api->ConnectionClose(static_cast<HQUIC>(conn_));
      delete cc;
    } else if (flag) {
      // The callback retires the connection now; the sink belongs to us, so it
      // must not go away before the callback is through with it.
      for (int i = 0; i < 500 && !flag->load(); ++i) Sleep(5);
    }
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
  // msquic does not expose the negotiated/path limit. The real ceiling comes
  // from the client's Hello (max_datagram_size) and msquic's own PMTU
  // discovery, which refuses an oversized send; 1200 is the QUIC minimum
  // payload and always safe.
  (void)conn;
  return 1200;
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
  StreamCtx* sc = stream ? StreamOf(static_cast<HQUIC>(stream)) : nullptr;
  if (!sc) return false;
  {
    std::lock_guard<std::mutex> lk(sc->m);
    if (sc->closed) return false;
    if (sc->queued + len > kStreamQueueLimit) {
      WVD_LOGW("quic: stream backlog full (%zu bytes) — peer stopped reading", sc->queued);
      sc->closed = true;
      return false;
    }
    // One msquic send at a time, and no block larger than a QUIC_BUFFER.
    for (size_t off = 0; off < len; off += kStreamChunk) {
      const size_t n = (std::min)(kStreamChunk, len - off);
      sc->out.emplace_back(data + off, data + off + n);
      sc->queued += n;
    }
  }
  StreamPump(sc);
  return true;
}

size_t StreamBacklog(QuicStream stream) {
  StreamCtx* sc = stream ? StreamOf(static_cast<HQUIC>(stream)) : nullptr;
  if (!sc) return 0;
  std::lock_guard<std::mutex> lk(sc->m);
  return sc->queued;
}

bool SendDatagram(QuicConn conn, std::vector<uint8_t> data) {
  if (!conn || data.empty() || data.size() > 0xFFFF) return false;
  auto* block = new DatagramBlock();
  block->bytes = std::move(data);
  block->qb.Length = static_cast<UINT32>(block->bytes.size());
  block->qb.Buffer = block->bytes.data();
  const QUIC_STATUS st = Api()->DatagramSend(static_cast<HQUIC>(conn), &block->qb, 1,
                                             QUIC_SEND_FLAG_DGRAM_PRIORITY, block);
  if (QUIC_FAILED(st)) delete block;  // refused: nothing was sent, nothing tracked
  return QUIC_SUCCEEDED(st);
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
  sc->stream = stream;
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

// QUIC transport (proto/WIRE.md §2) on the vendored msquic stack.
//
// One wrapper for the pieces WIRE v1 needs and nothing else:
//   * a server: registration + listener + TLS from the persisted HostIdentity
//     (ALPN "wvd/1", 15 s idle, RFC 9221 datagrams);
//   * a client: used by the loopback regression test, so the host protocol is
//     tested without a headset;
//   * TLS exporter secrets (the pairing keys, §7) and the peer certificate
//     hash;
//   * ordered stream sends and datagram sends with correct buffer ownership
//     (msquic keeps using a send buffer until it reports the send finished, so
//     every send takes ownership of a heap block).
//
// Threading: every callback runs on an msquic worker thread, and different
// connections are served concurrently. Sinks must not block; heavy work goes
// to the session's own threads. Send helpers are safe from any thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wvd/platform.h"

namespace wvd {

class HostIdentity;

// Opaque msquic handles (HQUIC). Valid only until the matching OnQuicClosed /
// OnQuicStreamClosed; never use one after that.
using QuicConn = void*;
using QuicStream = void*;

inline constexpr const char* kQuicAlpn = "wvd/1";
inline constexpr uint16_t kQuicDefaultPort = 47800;

// Called from msquic worker threads.
class IQuicSink {
 public:
  virtual ~IQuicSink() = default;
  // TLS handshake finished; exporter secrets are readable from here on.
  // `peerCertSha256` is empty on the server (clients present no certificate).
  virtual void OnQuicConnected(QuicConn conn, const std::vector<uint8_t>& peerCertSha256) = 0;
  // The peer opened a bidirectional stream. It reads automatically; bytes
  // arrive through OnQuicStreamData.
  virtual void OnQuicStream(QuicConn conn, QuicStream stream) = 0;
  virtual void OnQuicStreamData(QuicConn conn, QuicStream stream, const uint8_t* data,
                                size_t len) = 0;
  // The stream is gone (peer reset, or the connection closed). Drop all state
  // keyed by it.
  virtual void OnQuicStreamClosed(QuicConn conn, QuicStream stream) = 0;
  virtual void OnQuicDatagram(QuicConn conn, const uint8_t* data, size_t len) = 0;
  // The connection is finished; its handle is dead when this returns.
  virtual void OnQuicClosed(QuicConn conn, uint32_t status) = 0;
};

// Shared msquic state (opened once per process; every owner may come and go).
bool QuicSharedOpen(std::string* error);
void QuicSharedClose();
// HQUIC registration, valid once QuicSharedOpen() succeeded.
void* QuicRegistration();

struct QuicServerOptions {
  uint16_t port = kQuicDefaultPort;
  const char* alpn = kQuicAlpn;
  uint32_t idleTimeoutMs = 15000;       // WIRE.md §2; the client pings every 1 s
  uint32_t handshakeIdleTimeoutMs = 10000;
  uint16_t peerBidiStreamCount = 8;     // control + input, room to spare
  bool loopbackOnly = false;            // tests: bind 127.0.0.1
};

// Listens for one host session at a time; every accepted connection is
// reported to the sink and becomes whoever's session it wants to be.
class QuicServer {
 public:
  QuicServer();
  ~QuicServer();
  QuicServer(const QuicServer&) = delete;
  QuicServer& operator=(const QuicServer&) = delete;

  bool Start(const QuicServerOptions& options, HostIdentity& identity, IQuicSink& sink,
             std::string* error);
  void Stop();
  bool running() const { return listener_ != nullptr; }
  uint16_t port() const { return port_; }

 private:
  void* listener_ = nullptr;  // HQUIC
  void* config_ = nullptr;    // HQUIC
  uint16_t port_ = 0;
};

// Client side, for the loopback regression test (the shipped product is a host).
class QuicClient {
 public:
  QuicClient();
  ~QuicClient();
  QuicClient(const QuicClient&) = delete;
  QuicClient& operator=(const QuicClient&) = delete;

  // `verifyPeerCertSha256`: when non-empty the connection is refused unless the
  // peer's leaf certificate hashes to it (a paired client, WIRE.md §7). Empty
  // means trust-on-first-use.
  bool Connect(const std::string& host, uint16_t port, IQuicSink& sink, std::string* error,
               const std::vector<uint8_t>& verifyPeerCertSha256 = {}, uint32_t idleTimeoutMs = 15000);
  void Close();
  bool connected() const { return conn_ != nullptr; }
  QuicConn conn() const { return conn_; }

 private:
  void* config_ = nullptr;
  QuicConn conn_ = nullptr;
};

// ---- per-connection operations (valid handles only) ------------------------

namespace quic {

// TLS exporter secret (WIRE.md §7): 32 bytes for "EXPORTER-wvd-sas-v1",
// "EXPORTER-wvd-auth-v1" and "EXPORTER-wvd-pair-v1".
bool ExportKeyingMaterial(QuicConn conn, const char* label, std::vector<uint8_t>& out);
// Largest datagram payload the path can carry right now (0 = datagrams off).
uint32_t MaxDatagramPayload(QuicConn conn);
// "a.b.c.d:port" of the peer, for logs.
std::string PeerAddress(QuicConn conn);

// Queues bytes on a stream. One send is outstanding per stream at a time, so
// frames leave in the order they were queued (WIRE.md §3 needs that). Copies
// the data. Returns false when the queue is full (a peer that stopped reading)
// or the stream is gone.
bool SendStream(QuicStream stream, const uint8_t* data, size_t len);
// Number of bytes waiting to leave on that stream.
size_t StreamBacklog(QuicStream stream);

// Sends one datagram; takes ownership of `data`. Returns false when datagrams
// are unavailable or the peer is not reading — the caller counts a loss and
// moves on (media recovers with FEC/NACK).
bool SendDatagram(QuicConn conn, std::vector<uint8_t> data);

// Opens a client-initiated bidirectional stream; the sink's OnQuicStream is
// NOT called for it (you own it). Returns null when the peer allows none.
QuicStream OpenStream(QuicConn conn);

// Application-level close with a reason code.
void CloseConnection(QuicConn conn, uint32_t appErrorCode);
void CloseStream(QuicStream stream, uint32_t appErrorCode);

}  // namespace quic
}  // namespace wvd

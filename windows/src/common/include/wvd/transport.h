// Transport abstraction (§6.3).
//
// P0 uses TCP for the dev loop (localhost / LAN functional bring-up);
// backend A's production transport is QUIC (msquic ↔ Network.framework)
// with datagrams for media/cursor/audio and streams for control/input.
// The interface keeps both stream and datagram semantics so msquic drops in
// without touching the session layer.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "wvd/platform.h"
#include "wvd/protocol.h"

namespace wvd {

// Framed, reliable byte channel (one packet per Send).
class IReliableChannel {
 public:
  virtual ~IReliableChannel() = default;
  // Mark the underlying socket as interactive video (qWAVE) if applicable.
  virtual void MarkInteractiveVideo() {}
  virtual bool Send(const uint8_t* data, size_t len) = 0;
  bool SendPacket(const std::vector<uint8_t>& framed) {
    return Send(framed.data(), framed.size());
  }
  // Blocks up to timeoutMs; returns full packet bytes (with header) or nullopt.
  virtual std::optional<std::vector<uint8_t>> RecvPacket(uint32_t timeoutMs) = 0;
  virtual void Close() = 0;
  // False once the peer is gone. Lets the recv loop stop instead of spinning on
  // an empty dead socket, and lets the host re-accept (§4.2 reconnect).
  virtual bool IsConnected() const { return true; }
};

class TcpSocketHandle {
 public:
  TcpSocketHandle() = default;
  explicit TcpSocketHandle(uintptr_t s) : s_(s) {}
  TcpSocketHandle(TcpSocketHandle&& o) noexcept : s_(o.s_) { o.s_ = ~0ull; }
  TcpSocketHandle& operator=(TcpSocketHandle&& o) noexcept {
    if (this != &o) { CloseRaw(); s_ = o.s_; o.s_ = ~0ull; }
    return *this;
  }
  ~TcpSocketHandle() { CloseRaw(); }
  TcpSocketHandle(const TcpSocketHandle&) = delete;
  TcpSocketHandle& operator=(const TcpSocketHandle&) = delete;

  uintptr_t get() const { return s_; }
  bool valid() const { return s_ != ~0ull; }
  void reset() { CloseRaw(); }

 private:
  void CloseRaw();
  uintptr_t s_ = ~0ull;
};

// Client side.
class TcpChannel : public IReliableChannel {
 public:
  TcpChannel();
  ~TcpChannel() override;

  bool Connect(const char* host, uint16_t port, uint32_t timeoutMs);
  // Mark socket as interactive video for WMM (qWAVE, §6.3).
  void MarkInteractiveVideo();

  bool Send(const uint8_t* data, size_t len) override;
  std::optional<std::vector<uint8_t>> RecvPacket(uint32_t timeoutMs) override;
  void Close() override;
  bool IsConnected() const override { return connected_.load(); }
  // Peer address for logs ("a.b.c.d:port"), empty if unknown.
  const std::string& peer() const { return peer_; }

 private:
  friend class TcpListener;
  bool recvAll(uint8_t* dst, size_t n, int64_t deadlineQpc);
  // Send and receive run on different threads and must NOT share one lock: a
  // receive waiting 200 ms for input used to hold the only socket mutex, so
  // every video packet queued behind it waited too. Each direction has its
  // own lock. Close() only shutdown()s the socket (the handle stays valid, so
  // there is no use-after-close); closesocket happens in the destructor, when
  // neither direction can still be using it.
  std::mutex sendMutex_;
  std::mutex recvMutex_;
  TcpSocketHandle sock_;
  std::atomic<bool> connected_{false};
  std::string peer_;
};

// Server side: listen + accept (P0 accepts one peer at a time).
class TcpListener {
 public:
  TcpListener();
  ~TcpListener();
  // bindAddr: dotted IPv4 to listen on. "0.0.0.0" (every interface) is what a
  // headset on the LAN needs; "127.0.0.1" keeps local tests off the network.
  bool Start(uint16_t port, int backlog = 2, const char* bindAddr = "0.0.0.0");
  std::unique_ptr<TcpChannel> Accept(uint32_t timeoutMs);
  void Close();
  uint16_t port() const { return port_; }

 private:
  TcpSocketHandle sock_;
  uint16_t port_ = 0;
};

// Winsock global init (refcounted).
class WinsockGuard {
 public:
  WinsockGuard();
  ~WinsockGuard();
  bool ok() const { return ok_; }

 private:
  bool ok_;
};

}  // namespace wvd

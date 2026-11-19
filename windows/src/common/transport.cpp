#include "wvd/transport.h"

#include <ws2tcpip.h>

#include "wvd/logging.h"
#include "wvd/qos_marker.h"
#include "wvd/time.h"

#pragma comment(lib, "ws2_32.lib")

namespace wvd {

void TcpSocketHandle::CloseRaw() {
  if (s_ != ~0ull) {
    closesocket(static_cast<SOCKET>(s_));
    s_ = ~0ull;
  }
}

static int g_ws_count = 0;
static SRWLOCK g_ws_lock = SRWLOCK_INIT;

static bool WsGlobalInit() {
  AcquireSRWLockExclusive(&g_ws_lock);
  if (g_ws_count++ == 0) {
    WSADATA d;
    if (WSAStartup(MAKEWORD(2, 2), &d) != 0) {
      --g_ws_count;
      ReleaseSRWLockExclusive(&g_ws_lock);
      return false;
    }
  }
  ReleaseSRWLockExclusive(&g_ws_lock);
  return true;
}

static void WsGlobalCleanup() {
  AcquireSRWLockExclusive(&g_ws_lock);
  if (--g_ws_count == 0) WSACleanup();
  ReleaseSRWLockExclusive(&g_ws_lock);
}

static std::string AddrToString(const sockaddr_in& a) {
  char ip[INET_ADDRSTRLEN] = {};
  InetNtopA(AF_INET, &a.sin_addr, ip, sizeof(ip));
  return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

WinsockGuard::WinsockGuard() : ok_(WsGlobalInit()) {}
WinsockGuard::~WinsockGuard() { if (ok_) WsGlobalCleanup(); }

TcpChannel::TcpChannel() { WsGlobalInit(); }
TcpChannel::~TcpChannel() {
  Close();
  {
    // Both directions are idle once their locks can be taken; only then is the
    // handle actually released.
    std::lock_guard<std::mutex> ls(sendMutex_);
    std::lock_guard<std::mutex> lr(recvMutex_);
    if (sock_.valid()) QosUnmarkSocket(static_cast<SOCKET>(sock_.get()));
    sock_.reset();
  }
  WsGlobalCleanup();
}

bool TcpChannel::Connect(const char* host, uint16_t port, uint32_t timeoutMs) {
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return false;

  sockaddr_in addr{};
  if (InetPtonA(AF_INET, host, &addr.sin_addr) != 1) {
    WVD_LOGE("connect: '%s' is not a dotted IPv4 address", host);
    closesocket(s);
    return false;
  }
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);

  u_long nonblock = 1;
  ioctlsocket(s, FIONBIO, &nonblock);
  int r = connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  if (r == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
    closesocket(s);
    return false;
  }
  if (r == SOCKET_ERROR) {
    fd_set wf, ef;
    FD_ZERO(&wf);
    FD_ZERO(&ef);
    FD_SET(s, &wf);
    FD_SET(s, &ef);
    timeval tv{long(timeoutMs / 1000), long((timeoutMs % 1000) * 1000)};
    if (select(0, nullptr, &wf, &ef, &tv) != 1 || FD_ISSET(s, &ef)) {
      closesocket(s);
      return false;
    }
  }
  int err = 0;
  int len = sizeof(err);
  getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
  if (err != 0) {
    closesocket(s);
    return false;
  }
  BOOL nodelay = TRUE;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay),
             sizeof(nodelay));
  sock_ = TcpSocketHandle(s);
  peer_ = AddrToString(addr);
  connected_ = true;
  return true;
}

void TcpChannel::MarkInteractiveVideo() {
  if (!connected_) return;
  QosMarkInteractiveVideo(static_cast<SOCKET>(sock_.get()));
  // Bound what the kernel may hold for a slow link. With the default
  // auto-tuned send buffer several pictures disappear into the socket before
  // send() blocks, so the session learns about a slow link late and every
  // queued picture is latency the viewer sees. ~35 ms of a 60 Mbit/s stream.
  const int sendBuffer = 256 * 1024;
  setsockopt(static_cast<SOCKET>(sock_.get()), SOL_SOCKET, SO_SNDBUF,
             reinterpret_cast<const char*>(&sendBuffer), sizeof(sendBuffer));
}

bool TcpChannel::Send(const uint8_t* data, size_t len) {
  std::lock_guard<std::mutex> lk(sendMutex_);
  if (!connected_) return false;
  SOCKET s = static_cast<SOCKET>(sock_.get());
  size_t off = 0;
  while (off < len) {
    int n = send(s, reinterpret_cast<const char*>(data + off), int(len - off), 0);
    if (n == SOCKET_ERROR) {
      int e = WSAGetLastError();
      if (e == WSAEWOULDBLOCK) {
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(s, &wf);
        timeval tv{2, 0};
        if (select(0, nullptr, &wf, nullptr, &tv) != 1) {
          // 2 s without send buffer space: the peer stopped reading.
          Close();
          return false;
        }
        continue;
      }
      Close();
      return false;
    }
    off += size_t(n);
  }
  return true;
}

bool TcpChannel::recvAll(uint8_t* dst, size_t n, int64_t deadlineQpc) {
  SOCKET s = static_cast<SOCKET>(sock_.get());
  size_t off = 0;
  while (off < n) {
    // A peer that stops mid-packet must not wedge the reader (and with it
    // Stop()): every read is bounded by the caller's deadline.
    if (QpcNow() >= deadlineQpc) return false;
    if (!connected_) return false;
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(s, &rf);
    timeval tv{0, 20 * 1000};
    int r = select(0, &rf, nullptr, nullptr, &tv);
    if (r == 0) continue;
    if (r == SOCKET_ERROR) return false;
    int got = recv(s, reinterpret_cast<char*>(dst + off), int(n - off), 0);
    if (got == 0) return false;  // peer closed
    if (got == SOCKET_ERROR) {
      if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
      return false;
    }
    off += size_t(got);
  }
  return true;
}

std::optional<std::vector<uint8_t>> TcpChannel::RecvPacket(uint32_t timeoutMs) {
  std::lock_guard<std::mutex> lk(recvMutex_);
  if (!connected_) return std::nullopt;

  // Wait for the first byte with the caller's timeout; a timeout here is not
  // an error (nothing to read). Once a header has started, the rest of the
  // packet must arrive within a fixed bound or the stream is broken.
  uint8_t hdrBuf[sizeof(PacketHeader)];
  SOCKET s = static_cast<SOCKET>(sock_.get());
  {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(s, &rf);
    timeval tv{long(timeoutMs / 1000), long((timeoutMs % 1000) * 1000)};
    int r = select(0, &rf, nullptr, nullptr, &tv);
    if (r == 0) return std::nullopt;
    if (r == SOCKET_ERROR) {
      Close();
      return std::nullopt;
    }
  }
  const int64_t deadline = QpcNow() + MsToQpc(2000.0);
  if (!recvAll(hdrBuf, sizeof(hdrBuf), deadline)) {
    Close();
    return std::nullopt;
  }

  PacketHeader h;
  std::memcpy(&h, hdrBuf, sizeof(h));
  if (h.magic != kWireMagic || h.length > 16u * 1024 * 1024) {
    WVD_LOGW("tcp: bad packet header from %s — closing", peer_.c_str());
    Close();
    return std::nullopt;
  }

  std::vector<uint8_t> out(sizeof(PacketHeader) + h.length);
  std::memcpy(out.data(), hdrBuf, sizeof(hdrBuf));
  if (h.length && !recvAll(out.data() + sizeof(PacketHeader), h.length, deadline)) {
    Close();
    return std::nullopt;
  }
  return out;
}

void TcpChannel::Close() {
  // Safe from any thread and while the other direction is mid-call: shutdown
  // wakes a blocked select/recv/send, the handle itself stays valid until the
  // destructor.
  if (connected_.exchange(false) && sock_.valid())
    shutdown(static_cast<SOCKET>(sock_.get()), SD_BOTH);
}

TcpListener::TcpListener() { WsGlobalInit(); }
TcpListener::~TcpListener() { Close(); WsGlobalCleanup(); }

bool TcpListener::Start(uint16_t port, int backlog, const char* bindAddr) {
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return false;
  // Exclusive bind: SO_REUSEADDR on Windows would let another process bind the
  // same port and steal the headset's connection.
  BOOL excl = TRUE;
  setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl),
             sizeof(excl));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  if (!bindAddr || InetPtonA(AF_INET, bindAddr, &addr.sin_addr) != 1)
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
    WVD_LOGE("bind %s:%u failed wsa=%d", bindAddr ? bindAddr : "0.0.0.0", port,
             WSAGetLastError());
    closesocket(s);
    return false;
  }
  if (listen(s, backlog) == SOCKET_ERROR) {
    closesocket(s);
    return false;
  }
  sock_ = TcpSocketHandle(s);
  port_ = port;
  return true;
}

std::unique_ptr<TcpChannel> TcpListener::Accept(uint32_t timeoutMs) {
  if (!sock_.valid()) return nullptr;
  SOCKET s = static_cast<SOCKET>(sock_.get());
  fd_set rf;
  FD_ZERO(&rf);
  FD_SET(s, &rf);
  timeval tv{long(timeoutMs / 1000), long((timeoutMs % 1000) * 1000)};
  if (select(0, &rf, nullptr, nullptr, &tv) != 1) return nullptr;
  sockaddr_in peer{};
  int plen = sizeof(peer);
  SOCKET c = accept(s, reinterpret_cast<sockaddr*>(&peer), &plen);
  if (c == INVALID_SOCKET) return nullptr;
  BOOL nodelay = TRUE;
  setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
  u_long nonblock = 1;
  ioctlsocket(c, FIONBIO, &nonblock);
  auto ch = std::make_unique<TcpChannel>();
  ch->sock_ = TcpSocketHandle(c);
  ch->peer_ = AddrToString(peer);
  ch->connected_ = true;
  return ch;
}

void TcpListener::Close() { sock_.reset(); }

}  // namespace wvd

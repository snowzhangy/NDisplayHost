#include "wvd/ipc.h"

#include <sddl.h>
#include <wtsapi32.h>

#include <atomic>
#include <thread>
#include <vector>

#include "wvd/logging.h"
#include "wvd/protocol.h"

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "advapi32.lib")

namespace wvd {

namespace {

constexpr DWORD kMaxIpcPayload = 4u * 1024 * 1024;
constexpr DWORD kBodyTimeoutMs = 2000;  // once a header arrived, the rest must follow
std::atomic<uint64_t> g_nextConnection{1};
thread_local uint64_t g_connectionId = 0;

// Overlapped I/O helpers. Each returns ERROR_SUCCESS, WAIT_TIMEOUT,
// ERROR_OPERATION_ABORTED (stop event fired) or the Win32 error of the I/O.
DWORD WaitIo(HANDLE h, OVERLAPPED& ov, DWORD& bytes, DWORD timeoutMs, HANDLE stopEvent) {
  HANDLE hs[2] = {ov.hEvent, stopEvent};
  const DWORD n = stopEvent ? 2 : 1;
  const DWORD w = WaitForMultipleObjects(n, hs, FALSE, timeoutMs);
  if (w == WAIT_OBJECT_0) {
    if (GetOverlappedResult(h, &ov, &bytes, FALSE)) return ERROR_SUCCESS;
    return GetLastError();
  }
  // Timed out or stopping: cancel and wait for the cancellation to land so the
  // OVERLAPPED/buffer are no longer referenced by the kernel.
  CancelIoEx(h, &ov);
  GetOverlappedResult(h, &ov, &bytes, TRUE);
  if (w == WAIT_OBJECT_0 + 1) return ERROR_OPERATION_ABORTED;
  if (w == WAIT_TIMEOUT) return WAIT_TIMEOUT;
  return GetLastError();
}

DWORD Remaining(ULONGLONG deadline) {
  if (deadline == ~0ull) return INFINITE;
  const ULONGLONG now = GetTickCount64();
  return now >= deadline ? 0 : DWORD(deadline - now);
}

ULONGLONG DeadlineFrom(DWORD timeoutMs) {
  return timeoutMs == INFINITE ? ~0ull : GetTickCount64() + timeoutMs;
}

DWORD ReadExact(HANDLE h, HANDLE ev, uint8_t* dst, DWORD n, DWORD timeoutMs,
                HANDLE stopEvent) {
  const ULONGLONG deadline = DeadlineFrom(timeoutMs);
  DWORD off = 0;
  while (off < n) {
    OVERLAPPED ov{};
    ov.hEvent = ev;
    ResetEvent(ev);
    DWORD got = 0;
    if (!ReadFile(h, dst + off, n - off, nullptr, &ov)) {
      const DWORD e = GetLastError();
      if (e != ERROR_IO_PENDING && e != ERROR_MORE_DATA) return e;
    }
    const DWORD r = WaitIo(h, ov, got, Remaining(deadline), stopEvent);
    if (r != ERROR_SUCCESS && r != ERROR_MORE_DATA) return r;
    if (got == 0) return ERROR_BROKEN_PIPE;
    off += got;
  }
  return ERROR_SUCCESS;
}

DWORD WriteAll(HANDLE h, HANDLE ev, const uint8_t* src, DWORD n, DWORD timeoutMs,
               HANDLE stopEvent) {
  const ULONGLONG deadline = DeadlineFrom(timeoutMs);
  DWORD off = 0;
  while (off < n) {
    OVERLAPPED ov{};
    ov.hEvent = ev;
    ResetEvent(ev);
    DWORD put = 0;
    if (!WriteFile(h, src + off, n - off, nullptr, &ov)) {
      const DWORD e = GetLastError();
      if (e != ERROR_IO_PENDING) return e;
    }
    const DWORD r = WaitIo(h, ov, put, Remaining(deadline), stopEvent);
    if (r != ERROR_SUCCESS) return r;
    if (put == 0) return ERROR_BROKEN_PIPE;
    off += put;
  }
  return ERROR_SUCCESS;
}

bool IsBrokenPipe(DWORD e) {
  return e == ERROR_BROKEN_PIPE || e == ERROR_NO_DATA || e == ERROR_PIPE_NOT_CONNECTED;
}

std::wstring TokenUserSid(HANDLE tok) {
  std::wstring out;
  DWORD len = 0;
  GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
  if (!len) return out;
  std::vector<uint8_t> buf(len);
  if (!GetTokenInformation(tok, TokenUser, buf.data(), len, &len)) return out;
  LPWSTR sid = nullptr;
  if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &sid)) {
    out = sid;
    LocalFree(sid);
  }
  return out;
}

constexpr wchar_t kSystemSid[] = L"S-1-5-18";

}  // namespace

// ---- server ----------------------------------------------------------------

struct PipeServer::Impl {
  std::wstring name;
  Handler handler;
  DisconnectHandler onDisconnect;
  HANDLE stopEvent = nullptr;
  std::vector<std::thread> threads;

  // Returns true when the server is stopping.
  bool ServeConnection(HANDLE h, HANDLE ev) {
    for (;;) {
      uint8_t hdrBuf[sizeof(PacketHeader)];
      // Waiting for the NEXT request has no timeout: clients keep the
      // connection for a whole session. Only an incomplete message is bounded.
      DWORD r = ReadExact(h, ev, hdrBuf, sizeof(hdrBuf), INFINITE, stopEvent);
      if (r == ERROR_OPERATION_ABORTED) return true;
      if (r != ERROR_SUCCESS) return false;

      // Validate the header on its own. (PacketView::Parse needs the whole
      // packet; calling it on just the header rejected every message that had
      // a payload, i.e. every IPC command.)
      PacketHeader ph;
      std::memcpy(&ph, hdrBuf, sizeof(ph));
      if (ph.magic != kWireMagic || ph.version != kWireVersion || ph.length > kMaxIpcPayload)
        return false;

      std::vector<uint8_t> full(sizeof(PacketHeader) + ph.length);
      std::memcpy(full.data(), hdrBuf, sizeof(hdrBuf));
      if (ph.length) {
        r = ReadExact(h, ev, full.data() + sizeof(PacketHeader), ph.length, kBodyTimeoutMs,
                      stopEvent);
        if (r == ERROR_OPERATION_ABORTED) return true;
        if (r != ERROR_SUCCESS) return false;
      }

      auto resp = handler(full.data(), full.size());
      // Every request gets a response; a handler that has none closes the
      // connection so the client fails fast instead of waiting for a timeout.
      if (!resp || resp->empty()) return false;
      r = WriteAll(h, ev, resp->data(), DWORD(resp->size()), kBodyTimeoutMs, stopEvent);
      if (r == ERROR_OPERATION_ABORTED) return true;
      if (r != ERROR_SUCCESS) return false;
    }
  }

  void InstanceLoop(HANDLE hPipe) {
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    while (ev && WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0) {
      OVERLAPPED ov{};
      ov.hEvent = ev;
      ResetEvent(ev);
      bool connected = false;
      if (ConnectNamedPipe(hPipe, &ov)) {
        connected = true;
      } else {
        const DWORD e = GetLastError();
        if (e == ERROR_PIPE_CONNECTED) {
          connected = true;
        } else if (e == ERROR_IO_PENDING) {
          DWORD dummy = 0;
          const DWORD r = WaitIo(hPipe, ov, dummy, INFINITE, stopEvent);
          if (r == ERROR_OPERATION_ABORTED) break;
          connected = r == ERROR_SUCCESS;
        } else if (e != ERROR_NO_DATA) {
          WVD_LOGW("ConnectNamedPipe(%ls) failed gle=%lu", name.c_str(), e);
          if (WaitForSingleObject(stopEvent, 100) == WAIT_OBJECT_0) break;
        }
      }
      bool stopping = false;
      if (connected) {
        g_connectionId = g_nextConnection.fetch_add(1);
        stopping = ServeConnection(hPipe, ev);
        if (onDisconnect) onDisconnect();
        g_connectionId = 0;
      }
      DisconnectNamedPipe(hPipe);
      if (stopping) break;
    }
    if (ev) CloseHandle(ev);
    CloseHandle(hPipe);
  }
};

PipeServer::PipeServer() : impl_(std::make_unique<Impl>()) {}
PipeServer::~PipeServer() { Stop(); }

void PipeServer::SetDisconnectHandler(DisconnectHandler h) { impl_->onDisconnect = std::move(h); }
uint64_t PipeServer::CurrentConnectionId() { return g_connectionId; }

std::wstring PipeServer::ConsoleUserSidString() {
  std::wstring out;
  HANDLE tok = nullptr;
  const DWORD sess = WTSGetActiveConsoleSessionId();
  if (sess != 0xFFFFFFFF && WTSQueryUserToken(sess, &tok)) {
    out = TokenUserSid(tok);
    CloseHandle(tok);
  }
  return out;
}

std::wstring PipeServer::CurrentUserSidString() {
  std::wstring out;
  HANDLE tok = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
    out = TokenUserSid(tok);
    CloseHandle(tok);
  }
  return out;
}

bool PipeServer::CurrentProcessIsElevated() {
  HANDLE tok = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
  TOKEN_ELEVATION el{};
  DWORD len = 0;
  const bool ok = GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &len) != FALSE;
  CloseHandle(tok);
  return ok && el.TokenIsElevated;
}

std::wstring PipeServer::DefaultSddl() {
  std::wstring s = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)";
  const std::wstring self = CurrentUserSidString();
  const bool selfIsSystem = self == kSystemSid;
  const bool elevated = selfIsSystem || CurrentProcessIsElevated();
  if (!elevated && !self.empty()) s += L"(A;;GA;;;" + self + L")";

  // Client user: the interactive console user when we can resolve it (SYSTEM
  // service), otherwise the user this process runs as (elevated dev run).
  std::wstring client = ConsoleUserSidString();
  if (client.empty() && !selfIsSystem) client = self;
  if (client.empty()) {
    WVD_LOGW("pipe DACL: no console user resolvable — SYSTEM/Administrators only");
  } else if (client != kSystemSid && !(client == self && !elevated)) {
    s += L"(A;;GRGW;;;" + client + L")";
  }
  return s;
}

bool PipeServer::Start(const wchar_t* name, const wchar_t* sddl, Handler handler,
                       int maxInstances) {
  Stop();
  impl_->name = name;
  impl_->handler = std::move(handler);
  impl_->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!impl_->stopEvent) return false;

  PSECURITY_DESCRIPTOR sd = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd,
                                                            nullptr)) {
    WVD_LOGE("SDDL convert failed gle=%lu (%ls)", GetLastError(), sddl);
    Stop();
    return false;
  }
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.lpSecurityDescriptor = sd;

  if (maxInstances < 1) maxInstances = 1;
  std::vector<HANDLE> pipes;
  bool ok = true;
  for (int i = 0; i < maxInstances; ++i) {
    // The first instance claims the name: if anyone else already created a
    // pipe with this name (squatting to impersonate the server), fail loudly.
    const DWORD openMode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                           (i == 0 ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
    HANDLE hPipe = CreateNamedPipeW(
        name, openMode, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        DWORD(maxInstances), 64 * 1024, 64 * 1024, 0, &sa);
    if (hPipe == INVALID_HANDLE_VALUE) {
      const DWORD e = GetLastError();
      if (i == 0 && e == ERROR_ACCESS_DENIED)
        WVD_LOGE("CreateNamedPipe(%ls) denied — another process already owns this pipe "
                 "name (already running?)", name);
      else
        WVD_LOGE("CreateNamedPipe(%ls) instance %d failed gle=%lu", name, i, e);
      ok = false;
      break;
    }
    pipes.push_back(hPipe);
  }
  LocalFree(sd);
  if (!ok) {
    for (HANDLE h : pipes) CloseHandle(h);
    Stop();
    return false;
  }
  for (HANDLE h : pipes) impl_->threads.emplace_back([this, h] { impl_->InstanceLoop(h); });
  WVD_LOGI("pipe server listening: %ls (%d instances)", name, maxInstances);
  return true;
}

void PipeServer::Stop() {
  if (impl_->stopEvent) SetEvent(impl_->stopEvent);
  for (auto& t : impl_->threads)
    if (t.joinable()) t.join();
  impl_->threads.clear();
  if (impl_->stopEvent) {
    CloseHandle(impl_->stopEvent);
    impl_->stopEvent = nullptr;
  }
}

// ---- client ----------------------------------------------------------------

PipeClient::PipeClient() = default;
PipeClient::~PipeClient() {
  Close();
  if (ev_) CloseHandle(ev_);
}

bool PipeClient::Open(const wchar_t* name, uint32_t timeoutMs) {
  Close();
  name_ = name;
  if (!ev_) ev_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!ev_) return false;
  const ULONGLONG deadline = GetTickCount64() + timeoutMs;
  for (;;) {
    // SECURITY_IDENTIFICATION: a server can learn who we are but can never
    // impersonate us at a level that acts with our rights.
    HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                           nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      h_ = h;
      return true;
    }
    const DWORD e = GetLastError();
    const DWORD left = Remaining(deadline);
    if (left == 0) return false;
    if (e == ERROR_PIPE_BUSY) {
      WaitNamedPipeW(name, left);
      continue;
    }
    if (e == ERROR_FILE_NOT_FOUND) {  // server not up (yet)
      Sleep(left < 50 ? left : 50);
      continue;
    }
    return false;  // access denied etc.: retrying will not help
  }
}

bool PipeClient::RequestOnce(const std::vector<uint8_t>& framed,
                             std::vector<uint8_t>& response, uint32_t timeoutMs,
                             bool& brokenPipe) {
  brokenPipe = false;
  const ULONGLONG deadline = GetTickCount64() + timeoutMs;
  auto fail = [&](DWORD e) {
    brokenPipe = IsBrokenPipe(e);
    // A timed-out or half-read exchange leaves the stream out of sync; the only
    // safe recovery is a fresh connection.
    Close();
    return false;
  };
  DWORD r = WriteAll(h_, ev_, framed.data(), DWORD(framed.size()), Remaining(deadline), nullptr);
  if (r != ERROR_SUCCESS) return fail(r);

  uint8_t hdrBuf[sizeof(PacketHeader)];
  r = ReadExact(h_, ev_, hdrBuf, sizeof(hdrBuf), Remaining(deadline), nullptr);
  if (r != ERROR_SUCCESS) return fail(r);
  PacketHeader ph;
  std::memcpy(&ph, hdrBuf, sizeof(ph));
  if (ph.magic != kWireMagic || ph.length > kMaxIpcPayload) return fail(ERROR_INVALID_DATA);
  response.resize(sizeof(PacketHeader) + ph.length);
  std::memcpy(response.data(), hdrBuf, sizeof(hdrBuf));
  if (ph.length) {
    r = ReadExact(h_, ev_, response.data() + sizeof(PacketHeader), ph.length,
                  Remaining(deadline), nullptr);
    if (r != ERROR_SUCCESS) return fail(r);
  }
  return true;
}

bool PipeClient::Request(const std::vector<uint8_t>& framed,
                         std::vector<uint8_t>& response, uint32_t timeoutMs) {
  response.clear();
  if (!open()) {
    if (name_.empty() || !Open(name_.c_str(), timeoutMs)) return false;
  }
  bool broken = false;
  if (RequestOnce(framed, response, timeoutMs, broken)) return true;
  // Server restarted or dropped us between requests: one reconnect + retry.
  if (broken && !name_.empty() && Open(name_.c_str(), timeoutMs))
    return RequestOnce(framed, response, timeoutMs, broken);
  return false;
}

void PipeClient::Close() {
  if (h_ != INVALID_HANDLE_VALUE) {
    CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
  }
}

}  // namespace wvd

// Named-pipe IPC for local components (§4.7-4).
//
// Server: strict DACL (see DefaultSddl), local clients only, the first instance
// is created with FILE_FLAG_FIRST_PIPE_INSTANCE so nobody can squat the name
// before us, overlapped I/O so Stop() never hangs, and many requests per
// connection. Client: overlapped I/O with real timeouts, SQOS identification
// level (a rogue server cannot impersonate the client), one transparent reopen
// when the server dropped the connection.
//
// Message framing: one wire packet (PacketHeader + payload) per request and
// one per response.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wvd/platform.h"

namespace wvd {

class PipeServer {
 public:
  // Handler receives one framed packet; return bytes to write back (or none).
  // Handlers can run concurrently on different pipe instances.
  using Handler =
      std::function<std::optional<std::vector<uint8_t>>(const uint8_t* data, size_t len)>;
  // Called when a client connection ends (crash, close, error). The input
  // helper uses it to release held keys if the media worker dies (§4.4).
  using DisconnectHandler = std::function<void()>;

  PipeServer();
  ~PipeServer();

  void SetDisconnectHandler(DisconnectHandler h);
  // Unique per accepted connection, including reconnects on the same instance.
  // Valid inside request/disconnect callbacks only; zero outside callbacks.
  static uint64_t CurrentConnectionId();
  bool Start(const wchar_t* name, const wchar_t* sddl, Handler handler, int maxInstances = 4);
  void Stop();

  // DACL used by WVD pipes:
  //   SYSTEM + Administrators: GA (they host the servers)
  //   this process's user, only when NOT elevated: GA. A non-elevated dev
  //     process has BA deny-only, so without this it cannot create its own
  //     second pipe instance (that failed with ERROR_ACCESS_DENIED).
  //   the client user (console user, else this process's user): GRGW only —
  //     never GA, which includes FILE_CREATE_PIPE_INSTANCE / WRITE_DAC.
  static std::wstring DefaultSddl();

  // Active console user's SID (needs SYSTEM for WTSQueryUserToken); empty on failure.
  static std::wstring ConsoleUserSidString();
  // This process's user SID.
  static std::wstring CurrentUserSidString();
  static bool CurrentProcessIsElevated();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class PipeClient {
 public:
  PipeClient();
  ~PipeClient();
  PipeClient(const PipeClient&) = delete;
  PipeClient& operator=(const PipeClient&) = delete;

  bool Open(const wchar_t* name, uint32_t timeoutMs);
  // Sends one packet and waits for one response packet. If the connection was
  // dropped by the server, reopens once and retries.
  bool Request(const std::vector<uint8_t>& framed, std::vector<uint8_t>& response,
               uint32_t timeoutMs);
  void Close();
  bool open() const { return h_ != INVALID_HANDLE_VALUE; }

 private:
  bool RequestOnce(const std::vector<uint8_t>& framed, std::vector<uint8_t>& response,
                   uint32_t timeoutMs, bool& brokenPipe);
  HANDLE h_ = INVALID_HANDLE_VALUE;
  HANDLE ev_ = nullptr;
  std::wstring name_;
};

}  // namespace wvd

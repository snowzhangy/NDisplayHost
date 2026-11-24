#pragma once

#include <windows.h>
#include <sddl.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wvd::game_depth {

inline constexpr uint32_t kVersion = 2;
inline constexpr size_t kSlotCount = 3;
inline constexpr size_t kMaxPixels = 3840ull * 2160;
inline constexpr size_t kMaxDepthPixels = kMaxPixels / 16;

struct Request {
  uint64_t window = 0;
  int64_t token = 0;
  int64_t expires = 0;
  LUID adapter{};
  uint32_t width = 0, height = 0, fps = 0;
};

struct Frame {
  int64_t token = 0;
  int64_t qpc = 0;
  uint32_t width = 0, height = 0;
  bool depthValid = false;
  uint32_t slot = 0;
  uint64_t sequence = 0;
  uint64_t texture = 0;       // handle in the verified producer's process
  uint64_t textureId = 0;     // changes when a slot's texture is recreated
};

struct SharedFrames {
  uint32_t version = kVersion;
  uint32_t bytes = 0;
  Request request;
  Frame frame;
  uint64_t producerId = 0;
  uint64_t releaseFence = 0;  // handle in the verified producer's process
  std::array<uint64_t, kSlotCount> releases{};
  uint16_t depth[kMaxDepthPixels];
};

inline bool SupportedSize(uint32_t width, uint32_t height) {
  return width >= 256 && height >= 128 && !(width & 1) && !(height & 1) &&
         uint64_t(width) * height <= kMaxPixels;
}

inline bool SameAdapter(LUID a, LUID b) {
  return a.HighPart == b.HighPart && a.LowPart == b.LowPart;
}

class Handle {
 public:
  Handle() = default;
  explicit Handle(HANDLE handle) : handle_(handle) {}
  ~Handle() { Reset(); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  HANDLE Get() const { return handle_; }
  explicit operator bool() const { return handle_ && handle_ != INVALID_HANDLE_VALUE; }
  void Reset(HANDLE handle = nullptr) {
    if (*this) CloseHandle(handle_);
    handle_ = handle;
  }

 private:
  HANDLE handle_ = nullptr;
};

inline bool DuplicateFrom(HANDLE process, uint64_t value, Handle& result) {
  HANDLE duplicate = nullptr;
  if (!value || !DuplicateHandle(process, reinterpret_cast<HANDLE>(value), GetCurrentProcess(),
                                 &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) return false;
  result.Reset(duplicate);
  return true;
}

inline bool SameSession(DWORD pid) {
  DWORD peer = 0, local = 0;
  return ProcessIdToSessionId(pid, &peer) &&
         ProcessIdToSessionId(GetCurrentProcessId(), &local) && peer == local;
}

inline std::wstring PipeName(HWND window) {
  return L"\\\\.\\pipe\\NDisplayGameDepth-2-" + std::to_wstring(reinterpret_cast<uint64_t>(window));
}

// Only the rendezvous is named. Kernel handles to the section and mutex are
// transferred from the verified peer process, so there is no named frame
// object to pre-create, open or write from a second process.
struct Offer {
  uint32_t version = kVersion;
  DWORD hostPid = 0;
  uint64_t section = 0, mutex = 0;
};

class Mapping {
 public:
  ~Mapping() { Close(); }
  Mapping() = default;
  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;

  bool Create() {
    Close();
    mutex_.Reset(CreateMutexW(nullptr, FALSE, nullptr));
    section_.Reset(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                     sizeof(SharedFrames), nullptr));
    if (!Map()) return false;
    frames_->version = kVersion;
    frames_->bytes = sizeof(SharedFrames);
    return true;
  }

  Offer GetOffer() const {
    return {kVersion, GetCurrentProcessId(), reinterpret_cast<uint64_t>(section_.Get()),
            reinterpret_cast<uint64_t>(mutex_.Get())};
  }

  bool Import(HANDLE host, const Offer& offer) {
    Close();
    if (!DuplicateFrom(host, offer.section, section_) ||
        !DuplicateFrom(host, offer.mutex, mutex_) || !Map() ||
        frames_->version != kVersion || frames_->bytes != sizeof(SharedFrames)) {
      Close();
      return false;
    }
    return true;
  }

  explicit operator bool() const { return frames_ != nullptr; }
  void Close() {
    if (frames_) UnmapViewOfFile(frames_);
    frames_ = nullptr;
    section_.Reset();
    mutex_.Reset();
  }

  class Access {
   public:
    explicit Access(Mapping& mapping) : mapping_(mapping) {
      if (!mapping_.frames_) return;
      const DWORD wait = WaitForSingleObject(mapping_.mutex_.Get(), 0);
      if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
        frames = mapping_.frames_;
        if (wait == WAIT_ABANDONED) frames->frame = {};
      }
    }
    ~Access() { if (frames) ReleaseMutex(mapping_.mutex_.Get()); }
    Access(const Access&) = delete;
    Access& operator=(const Access&) = delete;
    SharedFrames* frames = nullptr;

   private:
    Mapping& mapping_;
  };

 private:
  bool Map() {
    if (mutex_ && section_)
      frames_ = static_cast<SharedFrames*>(MapViewOfFile(section_.Get(), FILE_MAP_WRITE,
                                                        0, 0, sizeof(SharedFrames)));
    if (!frames_) Close();
    return frames_ != nullptr;
  }

  Handle mutex_, section_;
  SharedFrames* frames_ = nullptr;
};

// Use a protected DACL with the host's exact user SID. PID checks on both
// ends further bind the rendezvous to one window's process and session.
inline HANDLE CreateOfferPipe(HWND window) {
  Handle token;
  HANDLE rawToken = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) return INVALID_HANDLE_VALUE;
  token.Reset(rawToken);
  DWORD length = 0;
  GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &length);
  if (!length) return INVALID_HANDLE_VALUE;
  std::vector<BYTE> data(length);
  if (!GetTokenInformation(token.Get(), TokenUser, data.data(), length, &length))
    return INVALID_HANDLE_VALUE;
  LPWSTR sid = nullptr;
  if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &sid))
    return INVALID_HANDLE_VALUE;
  const std::wstring sddl = std::wstring(L"D:P(A;;GA;;;") + sid + L")";
  LocalFree(sid);
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                           &descriptor, nullptr))
    return INVALID_HANDLE_VALUE;
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
  HANDLE pipe = CreateNamedPipeW(PipeName(window).c_str(),
      PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_MESSAGE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, sizeof(Offer), 0, 0, &security);
  const DWORD error = GetLastError();
  LocalFree(descriptor);
  SetLastError(error);
  return pipe;
}

inline bool OfferMapping(HANDLE pipe, DWORD gamePid, const Mapping& mapping) {
  // PIPE_NOWAIT can report success when it only put a disconnected instance
  // back into listening state. ERROR_PIPE_CONNECTED is the actual connection.
  if (ConnectNamedPipe(pipe, nullptr)) return false;
  const DWORD status = GetLastError();
  if (status == ERROR_NO_DATA) DisconnectNamedPipe(pipe);
  if (status != ERROR_PIPE_CONNECTED) return false;
  ULONG client = 0;
  if (!GetNamedPipeClientProcessId(pipe, &client) || client != gamePid) {
    DisconnectNamedPipe(pipe);
    return false;
  }
  const Offer offer = mapping.GetOffer();
  DWORD written = 0;
  if (WriteFile(pipe, &offer, sizeof(offer), &written, nullptr) && written == sizeof(offer))
    return true;
  DisconnectNamedPipe(pipe);
  return false;
}

// Game-side polling. Peek before reading on the render thread so a stalled
// client or a spoofed endpoint cannot block presentation.
inline bool ImportOffer(HANDLE pipe, Mapping& mapping, Handle& host) {
  ULONG owner = 0;
  DWORD available = 0;
  if (!GetNamedPipeServerProcessId(pipe, &owner) || !SameSession(owner) ||
      !PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available < sizeof(Offer))
    return false;
  Offer offer;
  DWORD read = 0;
  if (!ReadFile(pipe, &offer, sizeof(offer), &read, nullptr) || read != sizeof(offer) ||
      offer.version != kVersion || offer.hostPid != owner) return false;
  host.Reset(OpenProcess(PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, owner));
  return host && mapping.Import(host.Get(), offer);
}

}  // namespace wvd::game_depth

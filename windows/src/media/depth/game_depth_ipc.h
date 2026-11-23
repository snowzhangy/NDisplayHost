#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace wvd::game_depth {

// Local to the interactive Windows session. Installing the add-on in a game
// opts that executable in. The host publishes a short lease for one window.
inline constexpr wchar_t kMappingName[] = L"Local\\NDisplayGameDepthFrames-1";
inline constexpr wchar_t kMutexName[] = L"Local\\NDisplayGameDepthMutex-1";
inline constexpr size_t kMaxPixels = 3840ull * 2160;
inline constexpr size_t kMaxDepthPixels = kMaxPixels / 16;

struct Request {
  uint64_t window = 0;
  int64_t token = 0;  // QPC when this particular window/size lease started
  int64_t expires = 0;
  LUID adapter{};
  uint32_t width = 0, height = 0, fps = 0;
  DWORD owner = 0;
};

struct Frame {
  int64_t token = 0;
  int64_t qpc = 0;
  uint32_t width = 0, height = 0;
  bool depthValid = false;
};

// Dimensions are validated against the live request before reading pixels.
// Each record contains one BGRA image and its own quarter-scale inverse depth.
struct SharedFrames {
  Request request;
  Frame frame;
  uint32_t bgra[kMaxPixels];
  uint16_t depth[kMaxDepthPixels];
};

inline bool SupportedSize(uint32_t width, uint32_t height) {
  return width >= 256 && height >= 128 && !(width & 1) && !(height & 1) &&
         uint64_t(width) * height <= kMaxPixels;
}

inline bool SameAdapter(LUID a, LUID b) {
  return a.HighPart == b.HighPart && a.LowPart == b.LowPart;
}

// One producer slot, accessed only under a try-lock. A busy reader or writer
// drops an update without blocking the other process or the game render thread.
class Mapping {
 public:
  ~Mapping() { Close(); }
  Mapping() = default;
  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;

  bool Open(bool host) {
    if (frames_) return true;
    mutex_ = host ? CreateMutexW(nullptr, FALSE, kMutexName)
                  : OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, kMutexName);
    if (!mutex_) return false;
    const DWORD size = static_cast<DWORD>(sizeof(SharedFrames));
    mapping_ = host ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                         size, kMappingName)
                    : OpenFileMappingW(FILE_MAP_WRITE, FALSE, kMappingName);
    if (mapping_)
      frames_ = static_cast<SharedFrames*>(MapViewOfFile(mapping_, FILE_MAP_WRITE, 0, 0, size));
    if (!frames_) Close();
    return frames_ != nullptr;
  }

  class Access {
   public:
    explicit Access(Mapping& mapping) : mapping_(mapping) {
      if (!mapping_.frames_) return;
      const DWORD wait = WaitForSingleObject(mapping_.mutex_, 0);
      if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
        frames = mapping_.frames_;
        if (wait == WAIT_ABANDONED) frames->frame = {};
      }
    }
    ~Access() { if (frames) ReleaseMutex(mapping_.mutex_); }
    Access(const Access&) = delete;
    Access& operator=(const Access&) = delete;

    SharedFrames* frames = nullptr;

   private:
    Mapping& mapping_;
  };

 private:
  void Close() {
    if (frames_) UnmapViewOfFile(frames_);
    if (mapping_) CloseHandle(mapping_);
    if (mutex_) CloseHandle(mutex_);
    frames_ = nullptr;
    mapping_ = mutex_ = nullptr;
  }

  HANDLE mutex_ = nullptr, mapping_ = nullptr;
  SharedFrames* frames_ = nullptr;
};

}  // namespace wvd::game_depth

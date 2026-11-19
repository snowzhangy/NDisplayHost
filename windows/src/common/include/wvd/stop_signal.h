#pragma once
#include <atomic>
#include "wvd/platform.h"

namespace wvd {
// A launcher-owned event permits graceful shutdown without a visible console.
class StopSignal {
 public:
  ~StopSignal() {
    if (wait_) UnregisterWaitEx(wait_, INVALID_HANDLE_VALUE);
    if (event_) CloseHandle(event_);
  }
  bool Watch(const std::wstring& name, std::atomic<bool>& stop) {
    if (name.empty()) return true;
    event_ = OpenEventW(SYNCHRONIZE, FALSE, name.c_str());
    if (!event_) return false;
    if (WaitForSingleObject(event_, 0) == WAIT_OBJECT_0) { stop = true; return true; }
    return RegisterWaitForSingleObject(&wait_, event_, [](PVOID context, BOOLEAN) {
      static_cast<std::atomic<bool>*>(context)->store(true);
    }, &stop, INFINITE, WT_EXECUTEONLYONCE) != FALSE;
  }
 private:
  HANDLE event_ = nullptr, wait_ = nullptr;
};
}  // namespace wvd

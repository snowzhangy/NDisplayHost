// Sub-millisecond waits for the capture/encode cadence.
//
// Sleep() rounds up to the system timer tick (15.6 ms unless some process
// raised it), which is a whole 60 Hz frame of jitter. A high-resolution
// waitable timer (Windows 10 1803+) wakes within ~0.5 ms without changing the
// global tick; older systems fall back to a 1 ms timer period for the lifetime
// of this object.
#pragma once

#include "wvd/platform.h"
#include "wvd/time.h"

#include <timeapi.h>

#pragma comment(lib, "winmm.lib")

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace wvd {

class PrecisionSleeper {
 public:
  PrecisionSleeper() {
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                    TIMER_ALL_ACCESS);
    if (!timer_) {
      timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
      raisedPeriod_ = timeBeginPeriod(1) == TIMERR_NOERROR;
    }
  }
  ~PrecisionSleeper() {
    if (timer_) CloseHandle(timer_);
    if (raisedPeriod_) timeEndPeriod(1);
  }
  PrecisionSleeper(const PrecisionSleeper&) = delete;
  PrecisionSleeper& operator=(const PrecisionSleeper&) = delete;

  void SleepMs(double ms) {
    if (ms <= 0) return;
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(ms * 10000.0);  // relative, 100 ns units
    if (timer_ && due.QuadPart < 0 && SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE))
      WaitForSingleObject(timer_, INFINITE);
    else
      Sleep(static_cast<DWORD>(ms + 0.5));
  }

  void SleepUntilQpc(int64_t qpc) { SleepMs(QpcDeltaMs(QpcNow(), qpc)); }

 private:
  HANDLE timer_ = nullptr;
  bool raisedPeriod_ = false;
};

}  // namespace wvd

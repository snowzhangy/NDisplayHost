// QPC time helpers — one clock domain on the PC (§8). Headset offset is
// carried by Heartbeat (pcQpc ↔ headset clock) and reported with per-stage
// numbers.
#pragma once

#include <cstdint>

#include "wvd/platform.h"

namespace wvd {

inline int64_t QpcNow() {
  LARGE_INTEGER v;
  QueryPerformanceCounter(&v);
  return v.QuadPart;
}

inline double QpcFreq() {
  static LARGE_INTEGER f = [] {
    LARGE_INTEGER x;
    QueryPerformanceFrequency(&x);
    return x;
  }();
  return static_cast<double>(f.QuadPart);
}

inline int64_t MsToQpc(double ms) { return static_cast<int64_t>(ms * QpcFreq() / 1000.0); }
inline double QpcToMs(int64_t q) { return static_cast<double>(q) * 1000.0 / QpcFreq(); }
inline double QpcDeltaMs(int64_t from, int64_t to) { return QpcToMs(to - from); }

}  // namespace wvd

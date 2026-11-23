#include "depth/synthetic_depth.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wvd {

void SyntheticDepthSource::Generate(uint32_t mapW, uint32_t mapH, int32_t pointerX,
                                    int32_t pointerY, uint32_t pictureW, uint32_t pictureH,
                                    int64_t qpc) {
  if (!mapW || !mapH) return;
  DepthFrame f;
  f.qpc = qpc;
  f.seq = ++seq_;
  f.valid = dropEvery_ == 0 || (f.seq % dropEvery_) != 0;
  f.mapWidth = mapW;
  f.mapHeight = mapH;
  f.v.resize(size_t(mapW) * mapH);
  if (f.valid) {
    // Ramp 0..51400 (≈ 200/255 of the 16-bit domain) left→right: a plane
    // tilted away to the right. The square (raw 65535, the nearest value)
    // rides the pointer, so it must pop in front of every part of the plane.
    const uint32_t half = (std::max)(4u, mapW / 16u);
    const int32_t px = pictureW ? int32_t(int64_t(pointerX) * mapW / pictureW) : -100000;
    const int32_t py = pictureH ? int32_t(int64_t(pointerY) * mapH / pictureH) : -100000;
    for (uint32_t y = 0; y < mapH; ++y) {
      uint16_t* row = f.v.data() + size_t(y) * mapW;
      for (uint32_t x = 0; x < mapW; ++x) row[x] = uint16_t(uint64_t(x) * 51400 / mapW);
    }
    const int32_t x0 = (std::max)(0, px - int32_t(half)), x1 = (std::min)((int32_t)mapW, px + int32_t(half));
    const int32_t y0 = (std::max)(0, py - int32_t(half)), y1 = (std::min)((int32_t)mapH, py + int32_t(half));
    for (int32_t y = y0; y < y1; ++y)
      for (int32_t x = x0; x < x1; ++x) f.v[size_t(y) * mapW + x] = 65535;
  }
  std::lock_guard<std::mutex> lk(m_);
  hist_.push_back(std::move(f));
  while (hist_.size() > kHistory) hist_.pop_front();
}

bool SyntheticDepthSource::Find(int64_t qpc, DepthFrame& out) {
  std::lock_guard<std::mutex> lk(m_);
  for (auto it = hist_.rbegin(); it != hist_.rend(); ++it)
    if (it->qpc == qpc) {
      out = *it;
      return true;
    }
  return false;
}

void SyntheticDepthSource::Rebase(int64_t qpc) {
  std::lock_guard<std::mutex> lk(m_);
  while (!hist_.empty() && hist_.front().qpc < qpc) hist_.pop_front();
}

// ---------------------------------------------------------------------------
// DepthNormalizer
// ---------------------------------------------------------------------------

void DepthNormalizer::Observe(const uint16_t* texels, size_t count, double dtSec) {
  if (!texels || !count) return;
  // Histogram over the 16-bit raw domain (one pass), then read off the 1st
  // and 99th percentiles. 256 KB of counters, re-zeroed per frame: cheap next
  // to the percentile stretch it buys (full raw resolution below 1/255).
  static thread_local uint32_t hist[65536];
  std::memset(hist, 0, sizeof(hist));
  for (size_t i = 0; i < count; ++i) ++hist[texels[i]];
  const uint64_t n = count;
  const uint64_t loWant = n / 100;          // 1st percentile
  const uint64_t hiWant = n - n / 100;      // 99th percentile
  uint64_t cum = 0;
  float lo = 0.f, hi = 65535.f;
  for (uint32_t i = 0; i < 65536; ++i) {
    cum += hist[i];
    if (cum > loWant) { lo = float(i); break; }
  }
  cum = 0;
  for (uint32_t i = 0; i < 65536; ++i) {
    cum += hist[i];
    if (cum > hiWant) { hi = float(i); break; }
  }

  if (!primed_) {
    lo_ = lo;
    hi_ = hi;
    primed_ = true;
    // A flat first map (hi == lo) must not leave a zero-width interval: the
    // scale below would go infinite and the LUT fill would convert NaN to an
    // integer. Same minimum width as the steady-state guard.
    if (hi_ - lo_ < 1.f) hi_ = lo_ + 1.f;
  } else {
    // Exponential smoothing, ~0.5 s time constant; follow a scene cut at once
    // when the centre jumps by more than half the interval.
    const double dt = std::clamp(dtSec, 0.0, 0.25);
    const double alpha = 1.0 - std::exp(-dt / 0.5);
    const float centre = (lo_ + hi_) * 0.5f;
    const float half = (hi_ - lo_) * 0.5f;
    if (std::fabs((lo + hi) * 0.5f - centre) > half) {
      lo_ = lo;
      hi_ = hi;
    } else {
      lo_ = float(lo_ + alpha * (lo - lo_));
      hi_ = float(hi_ + alpha * (hi - hi_));
    }
    // Never let the interval collapse (a static grey scene): 1e-6 of the 0..1
    // domain, i.e. ~0.00026 raw levels — round up to a whole level to keep the
    // LUT monotone.
    if (hi_ - lo_ < 1.f) hi_ = lo_ + 1.f;
  }

  // hi_ - lo_ >= 1 on every path above: no divide-by-zero, no NaN.
  const float scale = 255.f / (hi_ - lo_);
  for (uint32_t i = 0; i < 65536; ++i) {
    const float q = (float(i) - lo_) * scale;
    lut_[i] = uint8_t(q <= 0.f ? 0u : (q >= 255.f ? 255u : uint32_t(q + 0.5f)));
  }
}

}  // namespace wvd

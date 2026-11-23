// Depth sources for the embedded-depth pipeline (game-depth plan §5–7).
//
// A depth source produces RAW inverse depth texels — larger = nearer — at the
// map resolution of the current layout, paired with the QPC stamp of the
// capture frame they belong to. The session owns the encoding half (§5):
// percentiles, smoothing, normalisation and packing into the coded picture.
// Keeping the split here is what lets H2's injected hook DLL stay tiny: it
// hands over texels, everything that can crash lives in the host.
//
// H1 provides a synthetic source (--depth-test-pattern): a left-to-right
// ramp that reads as a tilted plane, plus a nearer square that follows the
// pointer, so the headset side can be validated end to end before any hook
// exists.
#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace wvd {

// One raw depth map, keyed to the capture frame that produced it.
struct DepthFrame {
  int64_t qpc = 0;             // qpcCapture of the colour frame this pairs with
  uint64_t seq = 0;
  bool valid = false;          // false = the source missed this frame (DEPTH_VALID clear)
  uint32_t mapWidth = 0, mapHeight = 0;
  // RAW inverse depth, larger = nearer, in the FULL 16-bit domain. Sources
  // must not pre-quantise to 8 bits: the percentile stretch (§5) is what
  // maps raw → 8-bit strip texels, and quantising before it destroys the
  // precision the stretch exists to recover (the H2 hook hands over
  // R16_UNORM/R32_FLOAT — convert to 16-bit here, stretch later).
  std::vector<uint16_t> v;
};

// Small newest-wins history so the encode thread can find the map that pairs
// with the picture it is encoding (the application queue is depth 2).
class DepthSource {
 public:
  virtual ~DepthSource() = default;
  // Generate + append the map for capture frame (qpc). pointerX/pointerY are
  // in colour-picture pixels (may be outside when the pointer is elsewhere).
  virtual void Generate(uint32_t mapW, uint32_t mapH, int32_t pointerX, int32_t pointerY,
                        uint32_t pictureW, uint32_t pictureH, int64_t qpc) = 0;
  // Copy of the map paired with `qpc`. False when there is none.
  virtual bool Find(int64_t qpc, DepthFrame& out) = 0;
  // Drops everything older than `qpc` (the pipeline moved on).
  virtual void Rebase(int64_t qpc) = 0;
};

// --depth-test-pattern (§8 H1). Pure test content: no real depth anywhere.
class SyntheticDepthSource : public DepthSource {
 public:
  // dropEvery: clear `valid` on every Nth frame (0 = never). Drives the
  // H1 acceptance cases for DEPTH_VALID gaps (1 s and 3 s holds).
  explicit SyntheticDepthSource(uint32_t dropEvery = 0) : dropEvery_(dropEvery) {}

  void Generate(uint32_t mapW, uint32_t mapH, int32_t pointerX, int32_t pointerY,
                uint32_t pictureW, uint32_t pictureH, int64_t qpc) override;
  bool Find(int64_t qpc, DepthFrame& out) override;
  void Rebase(int64_t qpc) override;

 private:
  static constexpr size_t kHistory = 6;

  uint32_t dropEvery_;
  uint64_t seq_ = 0;
  std::mutex m_;
  std::deque<DepthFrame> hist_;
};

// §5 normalisation: per-frame 1st/99th percentiles of the raw texels,
// smoothed over time (time constant ~0.5 s) with an instant snap on a scene
// cut (the centre moves by more than half the interval). The headset runs its
// own percentile smoother on top; the host's only job is not to shake the
// scale, because every texel moves with lo/hi.
class DepthNormalizer {
 public:
  // Consumes one frame of raw 16-bit texels and updates the interval.
  // `dtSec` is the elapsed time since the previous applied frame (clamped
  // internally).
  void Observe(const uint16_t* texels, size_t count, double dtSec);
  // 65536-entry LUT: raw 16-bit -> 8-bit normalised (q = (v - lo)/(hi - lo)),
  // valid after the first Observe.
  const uint8_t* Lut() const { return lut_.data(); }
  // For diagnostics/logging: the current normalisation interval in raw units.
  float lo() const { return lo_; }
  float hi() const { return hi_; }

 private:
  float lo_ = 0.f, hi_ = 1.f;
  bool primed_ = false;
  std::vector<uint8_t> lut_ = std::vector<uint8_t>(65536, 0);
};

}  // namespace wvd

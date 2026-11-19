// Encoder target bitrate from sustained send-queue pressure, from how long encoded
// pictures wait for the send queue.
//
// A queue wait alone is noisy, and most waits are not a
// slow link: Wi-Fi pauses for tens of milliseconds (power save, airtime) and a
// keyframe takes a few frame times to leave. Both drain on their own, and the
// session already skips to the newest desktop picture while it waits, so
// cutting the bitrate for them only makes the picture softer — and with a
// slow climb back the target ratchets down over a few hiccups.
//
// So the target drops only for a link that keeps falling behind: at least
// three separate waits adding up to a quarter of the last second. It climbs
// back in steps of an eighth of the ceiling, once a second, after 1.5 s
// without such a backlog and while hardly anything waits.
#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>

namespace wvd {

class LinkRate {
 public:
  static constexpr double kWindowMs = 1000;
  static constexpr double kBehindMs = 250;   // waited in the window
  static constexpr size_t kBehindWaits = 3;  // separate waits in the window
  static constexpr double kQuietMs = 50;     // at most this waited to climb
  static constexpr double kMinWaitMs = 2;    // shorter is not a wait
  static constexpr double kCutEveryMs = 500;
  static constexpr double kRaiseAfterMs = 1500;
  static constexpr double kRaiseEveryMs = 1000;

  LinkRate(uint32_t maxKbps, uint32_t minKbps)
      : max_(maxKbps), min_((std::min)(minKbps, maxKbps)), target_(maxKbps) {}

  // A picture is about to be encoded after waiting `waitedMs` for the send
  // queue (0 = no wait); `nowMs` is any monotonic clock. Returns the new
  // target in kbps, or 0 to keep the current one.
  uint32_t OnPicture(double nowMs, double waitedMs) {
    const bool waiting = waitedMs >= kMinWaitMs;
    if (waiting) waits_.push_back({nowMs, waitedMs});
    Prune(nowMs);
    // Only a picture that is waiting now can cut: once the backlog is gone,
    // the waits still in the window are history, not a reason to cut again.
    if (waiting && waits_.size() >= kBehindWaits && waited_ >= kBehindMs) {
      lastBehindMs_ = nowMs;
      if (target_ > min_ && nowMs - lastChangeMs_ >= kCutEveryMs) {
        lastCutWaitedMs_ = waited_;
        target_ = (std::max)(min_, target_ / 4 * 3);
        lastChangeMs_ = nowMs;
        waits_.clear();  // the next cut needs fresh evidence at the new rate
        waited_ = 0;
        return target_;
      }
      return 0;
    }
    return MaybeRaise(nowMs);
  }

  // No picture for a while (a still desktop): the target may still climb.
  uint32_t OnIdle(double nowMs) {
    Prune(nowMs);
    return MaybeRaise(nowMs);
  }

  uint32_t target() const { return target_; }
  // What the last cut was based on: ms waited in its window.
  double lastCutWaitedMs() const { return lastCutWaitedMs_; }

 private:
  struct Wait {
    double atMs, waitedMs;
  };

  void Prune(double nowMs) {
    while (!waits_.empty() && nowMs - waits_.front().atMs > kWindowMs) waits_.pop_front();
    waited_ = 0;
    for (const auto& w : waits_) waited_ += w.waitedMs;
  }

  uint32_t MaybeRaise(double nowMs) {
    if (target_ >= max_ || waited_ > kQuietMs || nowMs - lastBehindMs_ < kRaiseAfterMs ||
        nowMs - lastChangeMs_ < kRaiseEveryMs)
      return 0;
    target_ = (std::min)(max_, target_ + (std::max)(500u, max_ / 8));
    lastChangeMs_ = nowMs;
    return target_;
  }

  uint32_t max_, min_, target_;
  std::deque<Wait> waits_;
  double waited_ = 0;
  double lastBehindMs_ = -1e18, lastChangeMs_ = -1e18;
  double lastCutWaitedMs_ = 0;
};

}  // namespace wvd

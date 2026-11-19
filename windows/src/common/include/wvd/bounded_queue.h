// Bounded queues with explicit overflow policy and depth reporting (§6.6).
// "No queue is unbounded, and every queue depth is reported."
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <chrono>

namespace wvd {

enum class OverflowPolicy {
  NewestWins,   // capture & presentation: drop the oldest, keep the newest
  DropOldest,   // same as NewestWins but counted differently (stall metric)
  Reject,       // control: sender is told the queue is full
};

template <class T>
class BoundedQueue {
 public:
  BoundedQueue(size_t capacityFrames, OverflowPolicy policy)
      : cap_(capacityFrames ? capacityFrames : 1), policy_(policy) {}

  // Returns false if the item was rejected (Reject policy, queue full).
  bool Push(T item) {
    std::lock_guard<std::mutex> lk(m_);
    if (items_.size() >= cap_) {
      if (policy_ == OverflowPolicy::Reject) {
        ++rejected;
        return false;
      }
      items_.pop_front();
      ++dropped;
    }
    items_.push_back(std::move(item));
    depthMax = depthMax < items_.size() ? items_.size() : depthMax;
    cv_.notify_one();
    return true;
  }

  std::optional<T> Pop(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(m_);
    if (!cv_.wait_for(lk, timeout, [this] { return !items_.empty(); }))
      return std::nullopt;
    T v = std::move(items_.front());
    items_.pop_front();
    return v;
  }

  // Newest-wins drain: returns the most recent item and discards the rest
  // (presentation path: never show stale frames).
  std::optional<T> PopLatest() {
    std::lock_guard<std::mutex> lk(m_);
    if (items_.empty()) return std::nullopt;
    T v = std::move(items_.back());
    dropped += items_.size() - 1;
    items_.clear();
    return v;
  }

  size_t Size() const {
    std::lock_guard<std::mutex> lk(m_);
    return items_.size();
  }

  struct Stats {
    size_t capacity;
    size_t depth;
    size_t depthMax;
    uint64_t dropped;
    uint64_t rejected;
  };
  Stats GetStats() const {
    std::lock_guard<std::mutex> lk(m_);
    return {cap_, items_.size(), depthMax, dropped, rejected};
  }
  void ResetStats() {
    std::lock_guard<std::mutex> lk(m_);
    depthMax = dropped = rejected = 0;
  }

 private:
  mutable std::mutex m_;
  std::condition_variable cv_;
  std::deque<T> items_;
  size_t cap_;
  OverflowPolicy policy_;
  size_t depthMax = 0;
  uint64_t dropped = 0;
  uint64_t rejected = 0;
};

}  // namespace wvd

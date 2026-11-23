#include "session/media_session.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
using namespace wvd;
struct Received {
  std::mutex mutex;
  std::vector<MediaPayloadHeader> pictures;
};
class SlowReceiver : public MediaChannel {
 public:
  explicit SlowReceiver(Received& received, DWORD delay = 80) : received_(received), delay_(delay) {}
  bool Send(const uint8_t* bytes, size_t count) override {
    auto packet = PacketView::Parse(bytes, count);
    if (!packet) return false;
    if (packet->kind() == PacketKind::Media) {
      auto picture = packet->Get<MediaPayloadHeader>();
      if (!picture) return false;
      {
        std::lock_guard<std::mutex> lock(received_.mutex);
        received_.pictures.push_back(*picture);
      }
      if (delay_) Sleep(delay_);
    }
    return open_;
  }
  std::optional<std::vector<uint8_t>> RecvPacket(uint32_t) override { Sleep(5); return std::nullopt; }
  size_t BacklogBytes() const override { return 0; }
  uint32_t VideoBitrateLimitKbps() const override { return 0; }
  uint32_t VideoFrameRateLimit() const override { return frameRate; }
  uint32_t VideoEpoch() const override { return epoch; }
  bool PreferVideoQuality() const override { return preferQuality; }
  bool IsConnected() const override { return open_; }
  void Close() override { open_ = false; }
  std::atomic<uint32_t> frameRate{0}, epoch{1};
  std::atomic<bool> preferQuality{true};
 private:
  Received& received_;
  DWORD delay_;
  std::atomic<bool> open_{true};
};

static std::vector<MediaPayloadHeader> Run(uint32_t backpressureMs, MediaSession::Snapshot* stats = nullptr) {
  SessionConfig config;
  config.codec = "null";
  config.fps = 60;
  config.deadlineMs = 0;  // every encoded picture is already late
  config.backpressureMs = backpressureMs;
  GenCounters generations;
  Received received;
  MediaSession session(config, generations);
  session.synthetic = true;
  CHECK(SUCCEEDED(session.Start(std::make_unique<SlowReceiver>(received))));
  Sleep(1800);
  if (stats) *stats = session.GetSnapshot();
  session.Stop();
  std::lock_guard<std::mutex> lock(received.mutex);
  return received.pictures;
}

int main() {
  // Default: a slow link holds the encoder back. Every encoded picture is
  // delivered in order, so the reference chain never breaks and no recovery
  // keyframe is spent on a link that is already behind.
  {
    MediaSession::Snapshot stats{};
    const auto pictures = Run(250, &stats);
    CHECK(stats.sendP95Ms >= 70 && stats.sendGapMaxMs >= 70);
    CHECK(stats.captureGapMaxMs > 0);
    CHECK(pictures.size() > 8);  // dependent frames cannot be deadline-dropped
    unsigned keys = 0;
    bool delta = false;
    for (size_t i = 0; i < pictures.size(); ++i) {
      if (i) CHECK(pictures[i].picSeq == pictures[i - 1].picSeq + 1);
      if (pictures[i].flags & 1) ++keys; else delta = true;
    }
    CHECK(pictures[0].flags & 1);
    CHECK(keys == 1 && delta);
  }
  // Without the hold-back the bounded send queue overflows; a dropped encoded
  // picture must be followed by a new reference chain.
  {
    const auto pictures = Run(0);
    CHECK(pictures.size() > 8);
    bool gap = false, delta = false;
    for (size_t i = 1; i < pictures.size(); ++i) {
      const auto& previous = pictures[i - 1];
      const auto& picture = pictures[i];
      CHECK(picture.picSeq > previous.picSeq);
      if (picture.picSeq != previous.picSeq + 1) {
        CHECK(picture.flags & 1);  // overflow recovers with a new reference chain
        gap = true;
      }
      if (!(picture.flags & 1)) delta = true;
    }
    CHECK(gap && delta);
  }
  {
    SessionConfig config;
    config.codec = "null";
    config.synWidth = 640;
    config.synHeight = 360;
    GenCounters generations;
    Received received;
    auto receiver = std::make_shared<SlowReceiver>(received, 0);
    receiver->frameRate = 30;
    MediaSession session(config, generations);
    session.synthetic = true;
    CHECK(SUCCEEDED(session.Start(receiver)));
    Sleep(200);
    const auto before = session.GetSnapshot();
    Sleep(1000);
    const auto capped = session.GetSnapshot();
    CHECK(capped.framesCaptured - before.framesCaptured >= 15);
    CHECK(capped.framesCaptured - before.framesCaptured <= 34);

    receiver->epoch = 2;
    Sleep(125);
    const auto hidden = session.GetSnapshot();
    Sleep(200);
    CHECK(session.GetSnapshot().framesEncoded == hidden.framesEncoded);
    receiver->preferQuality = false;
    receiver->epoch = 3;
    Sleep(300);
    session.Stop();

    std::lock_guard<std::mutex> lock(received.mutex);
    const auto first = std::find_if(received.pictures.begin(), received.pictures.end(),
                                    [](const auto& picture) { return picture.videoEpoch == 3; });
    CHECK(first != received.pictures.end() && (first->flags & 1));
    CHECK(first != received.pictures.begin() && first->codecGen > received.pictures.front().codecGen);
  }
  puts("slow-link delivery, frame-rate cap and pause/resume recovery passed");
}

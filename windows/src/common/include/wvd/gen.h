// Generations and identifiers (plan §6.6).
// Every wire unit carries {session_gen, layout_gen, codec_gen} plus
// picture/content counters so old-generation images can never be shown.
#pragma once

#include <atomic>
#include <cstdint>

namespace wvd {

struct GenSnapshot {
  uint64_t sessionGen = 0;   // connect/reconnect, media-worker restart
  uint64_t layoutGen = 0;    // monitor added/removed, mode or size change
  uint64_t codecGen = 0;     // codec, profile, size or encoder-tier change
  uint64_t contentVer = 0;   // every source desktop change
  uint64_t picSeq = 0;       // every encoded picture
};

// Thread-safe counters owned by the media session.
struct GenCounters {
  std::atomic<uint64_t> sessionGen{1};
  std::atomic<uint64_t> layoutGen{1};
  std::atomic<uint64_t> codecGen{1};
  std::atomic<uint64_t> contentVer{0};
  std::atomic<uint64_t> picSeq{0};
  std::atomic<uint64_t> inputGen{1};   // §4.4 input generations
  std::atomic<uint64_t> shapeVer{0};   // §6.2 cursor shape version

  GenSnapshot Snapshot() const {
    GenSnapshot s;
    s.sessionGen = sessionGen.load();
    s.layoutGen = layoutGen.load();
    s.codecGen = codecGen.load();
    s.contentVer = contentVer.load();
    s.picSeq = picSeq.load();
    return s;
  }
};

}  // namespace wvd

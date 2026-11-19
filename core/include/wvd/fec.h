// GF(2^8) Reed-Solomon erasure code + picture packetiser (WIRE.md §5).
//
// Normative: primitive polynomial 0x11D, systematic Cauchy matrix
//   parity_j = XOR_i C[j][i] * data_i,  C[j][i] = 1 / ((k + j) XOR i)
// The byte-for-byte reference is WVDAppleCore FEC/{ReedSolomon,Packetizer}.swift.
#pragma once

#include <cstdint>
#include <vector>

#include "wvd/wire1.h"

namespace wvd {
namespace fec {

namespace gf {
// a * b in GF(2^8).
uint8_t mul(uint8_t a, uint8_t b);
uint8_t inv(uint8_t a);  // a != 0
// dst ^= c * src over n bytes.
void mulAdd(uint8_t* dst, const uint8_t* src, uint8_t c, size_t n);
}  // namespace gf

// C[j][i] for parity row j, data column i (k data shards).
uint8_t Coefficient(int parityRow, int dataCol, int k);

// m parity shards from k data shards, each shardBytes long.
// data: k pointers; parity: m pointers (written). k + m <= 255.
bool Encode(const uint8_t* const* data, int k, uint8_t* const* parity, int m, size_t shardBytes);

// Reconstructs missing data shards in place from available parity.
//   present[i]: data shard i valid; missing ones are written.
//   parityRows/parityShards: d available parity rows (any d >= #missing).
// Requires parityCount >= number of missing data shards.
bool Reconstruct(int k, const uint8_t* const* data, const bool* present,
                 const int* parityRows, const uint8_t* const* parityShards, int parityCount,
                 size_t shardBytes);

// One coded picture split into datagrams (port of Packetizer.swift).
struct PacketizerConfig {
  int shardBytes = 1152;         // payload per shard; 48 + this must fit the path
  int maxBlockDataShards = 48;   // data shards per RS block
  double fecRatio = 0.10;        // parity per data shard, host-adapted
  int minParityPerBlock = 1;
  int maxParityPerBlock = 24;

  // Swift reference: shardBytes = max(256, min(usable, 1400) - 48).
  static PacketizerConfig ForDatagramSize(int usable) {
    PacketizerConfig c;
    int s = usable < 1400 ? usable : 1400;
    s -= 48;
    c.shardBytes = s > 256 ? s : 256;
    return c;
  }
};

struct FrameDescriptor {
  uint16_t monitorId = 1;
  uint32_t sessionGen = 0;
  uint32_t layoutGen = 0;
  uint32_t codecGen = 0;
  uint32_t picSeq = 0;
  uint32_t contentVer = 0;
  bool randomAccess = false;
  bool refinement = false;
  uint64_t captureTimeUs = 0;
  uint16_t deadlineBudgetMs = 100;
};

class PacketizedFrame {
 public:
  struct Block {
    int firstShard = 0;
    int k = 0;
    int m = 0;
  };

  PacketizedFrame() = default;
  PacketizedFrame(const FrameDescriptor& desc, const uint8_t* au, size_t auBytes,
                  const PacketizerConfig& cfg);

  const FrameDescriptor& descriptor() const { return desc_; }
  uint32_t frameBytes() const { return frameBytes_; }
  int shardBytes() const { return shardBytes_; }
  int totalDataShards() const { return totalDataShards_; }
  const std::vector<Block>& blocks() const { return blocks_; }

  // Data datagrams by global data-shard index.
  const std::vector<std::vector<uint8_t>>& dataDatagrams() const { return data_; }
  // Parity datagrams per block, row order; empty until ComputeParity().
  const std::vector<std::vector<std::vector<uint8_t>>>& parityDatagrams() const {
    return parity_;
  }

  // Interleaved across blocks (block0#0, block1#0, ...) so a burst costs each
  // block a few shards at most.
  std::vector<std::vector<uint8_t>> interleavedData() const;
  std::vector<std::vector<uint8_t>> interleavedParity() const;

  // Reed-Solomon parity for every block. Idempotent.
  void ComputeParity();

  // Copies of the requested data shards with RETRANSMIT set; empty request
  // means the whole picture.
  std::vector<std::vector<uint8_t>> Retransmission(const std::vector<uint16_t>& indices) const;

 private:
  dg::MediaShardHeader MakeHeader(uint8_t extraFlags, int blockIdx, int shardIndex) const;

  FrameDescriptor desc_;
  uint32_t frameBytes_ = 0;
  int shardBytes_ = 0;
  int totalDataShards_ = 0;
  std::vector<Block> blocks_;
  std::vector<std::vector<uint8_t>> data_;
  std::vector<std::vector<std::vector<uint8_t>>> parity_;
  std::vector<uint8_t> padded_;  // N * shardBytes, zero-padded AU
  PacketizerConfig cfg_;
};

}  // namespace fec
}  // namespace wvd

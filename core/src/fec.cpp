// GF(2^8) Cauchy Reed-Solomon + packetiser. See fec.h for the normative notes.
#include "wvd/fec.h"

#include <algorithm>
#include <cstring>

namespace wvd {
namespace fec {

namespace {

struct Tables {
  uint8_t exp[512];
  uint8_t log[256];
  uint8_t mul[65536];  // row = coefficient; one lookup per byte in the hot loop
  Tables() {
    unsigned x = 1;
    for (int i = 0; i < 255; i++) {
      exp[i] = static_cast<uint8_t>(x);
      log[x] = static_cast<uint8_t>(i);
      x <<= 1;
      if (x & 0x100) x ^= 0x11D;
    }
    for (int i = 255; i < 512; i++) exp[i] = exp[i - 255];
    for (int a = 1; a < 256; a++)
      for (int b = 1; b < 256; b++)
        mul[(a << 8) | b] = exp[log[a] + log[b]];
  }
};
const Tables& T() {
  static Tables t;
  return t;
}

}  // namespace

namespace gf {

uint8_t mul(uint8_t a, uint8_t b) {
  if (a == 0 || b == 0) return 0;
  return T().exp[T().log[a] + T().log[b]];
}
uint8_t inv(uint8_t a) {
  if (a == 0) return 0;  // caller error; avoid UB in a hot path
  return T().exp[255 - T().log[a]];
}
void mulAdd(uint8_t* dst, const uint8_t* src, uint8_t c, size_t n) {
  if (c == 0) return;
  if (c == 1) {
    for (size_t i = 0; i < n; i++) dst[i] ^= src[i];
    return;
  }
  const uint8_t* row = T().mul + (static_cast<size_t>(c) << 8);
  for (size_t i = 0; i < n; i++) dst[i] ^= row[src[i]];
}

}  // namespace gf

uint8_t Coefficient(int parityRow, int dataCol, int k) {
  return gf::inv(static_cast<uint8_t>(k + parityRow) ^ static_cast<uint8_t>(dataCol));
}

bool Encode(const uint8_t* const* data, int k, uint8_t* const* parity, int m, size_t shardBytes) {
  if (k <= 0 || k + m > 255) return false;
  for (int j = 0; j < m; j++) {
    std::memset(parity[j], 0, shardBytes);
    for (int i = 0; i < k; i++)
      gf::mulAdd(parity[j], data[i], Coefficient(j, i, k), shardBytes);
  }
  return true;
}

bool Reconstruct(int k, const uint8_t* const* data, const bool* present, const int* parityRows,
                 const uint8_t* const* parityShards, int parityCount, size_t shardBytes) {
  if (k <= 0) return false;
  std::vector<int> missing;
  for (int i = 0; i < k; i++)
    if (!present[i]) missing.push_back(i);
  const int d = static_cast<int>(missing.size());
  if (d == 0) return true;
  if (parityCount < d) return false;

  // 1. Remove the known data contributions from each used parity shard.
  std::vector<uint8_t> scratch(static_cast<size_t>(d) * shardBytes);
  for (int r = 0; r < d; r++) {
    uint8_t* dst = scratch.data() + static_cast<size_t>(r) * shardBytes;
    std::memcpy(dst, parityShards[r], shardBytes);
    for (int i = 0; i < k; i++)
      if (present[i])
        gf::mulAdd(dst, data[i], Coefficient(parityRows[r], i, k), shardBytes);
  }

  // 2. Invert the d x d Cauchy submatrix M[r][c] = C[row_r][missing_c].
  std::vector<uint8_t> a(static_cast<size_t>(d) * d), invM(static_cast<size_t>(d) * d);
  for (int r = 0; r < d; r++) {
    for (int c = 0; c < d; c++) a[r * d + c] = Coefficient(parityRows[r], missing[c], k);
    invM[r * d + r] = 1;
  }
  for (int col = 0; col < d; col++) {
    int pivot = -1;
    for (int r = col; r < d; r++)
      if (a[r * d + col] != 0) { pivot = r; break; }
    if (pivot < 0) return false;  // Cauchy submatrices are invertible; not our matrix
    if (pivot != col) {
      for (int c = 0; c < d; c++) {
        std::swap(a[pivot * d + c], a[col * d + c]);
        std::swap(invM[pivot * d + c], invM[col * d + c]);
      }
    }
    uint8_t pinv = gf::inv(a[col * d + col]);
    for (int c = 0; c < d; c++) {
      a[col * d + c] = gf::mul(a[col * d + c], pinv);
      invM[col * d + c] = gf::mul(invM[col * d + c], pinv);
    }
    for (int r = 0; r < d; r++) {
      if (r == col) continue;
      uint8_t f = a[r * d + col];
      if (f == 0) continue;
      for (int c = 0; c < d; c++) {
        a[r * d + c] ^= gf::mul(f, a[col * d + c]);
        invM[r * d + c] ^= gf::mul(f, invM[col * d + c]);
      }
    }
  }

  // 3. missing_c = sum_r invM[c][r] * scratch_r
  for (int c = 0; c < d; c++) {
    uint8_t* dst = const_cast<uint8_t*>(data[missing[c]]);
    std::memset(dst, 0, shardBytes);
    for (int r = 0; r < d; r++)
      gf::mulAdd(dst, scratch.data() + static_cast<size_t>(r) * shardBytes, invM[c * d + r],
                 shardBytes);
  }
  return true;
}

// ---- Packetizer -----------------------------------------------------------

namespace {

// Blocks of near-equal size: the first N mod B blocks get one extra shard.
std::vector<PacketizedFrame::Block> MakeBlocks(int n, const PacketizerConfig& cfg) {
  int blockCount = (n + cfg.maxBlockDataShards - 1) / cfg.maxBlockDataShards;
  if (blockCount < 1) blockCount = 1;
  int base = n / blockCount, extra = n % blockCount;
  std::vector<PacketizedFrame::Block> blocks;
  int first = 0;
  for (int b = 0; b < blockCount; b++) {
    PacketizedFrame::Block blk;
    blk.firstShard = first;
    blk.k = base + (b < extra ? 1 : 0);
    double mr = cfg.fecRatio * blk.k;
    int m = static_cast<int>(mr + 0.999999);  // ceil
    if (cfg.fecRatio <= 0) m = 0;
    if (m < cfg.minParityPerBlock) m = cfg.minParityPerBlock;
    if (m > cfg.maxParityPerBlock) m = cfg.maxParityPerBlock;
    if (m > 255 - blk.k) m = 255 - blk.k;
    blk.m = m;
    blocks.push_back(blk);
    first += blk.k;
  }
  return blocks;
}

}  // namespace

PacketizedFrame::PacketizedFrame(const FrameDescriptor& desc, const uint8_t* au, size_t auBytes,
                                 const PacketizerConfig& cfg)
    : desc_(desc), cfg_(cfg) {
  frameBytes_ = static_cast<uint32_t>(auBytes);
  shardBytes_ = cfg.shardBytes;
  int n = static_cast<int>((auBytes + shardBytes_ - 1) / shardBytes_);
  if (n < 1) n = 1;
  totalDataShards_ = n;
  blocks_ = MakeBlocks(n, cfg);

  uint8_t flags = 0;
  if (desc_.randomAccess) flags |= dg::MediaShardHeader::kRandomAccess;
  if (desc_.refinement) flags |= dg::MediaShardHeader::kRefinement;

  data_.resize(n);
  for (size_t bi = 0; bi < blocks_.size(); bi++) {
    const Block& blk = blocks_[bi];
    for (int i = 0; i < blk.k; i++) {
      int global = blk.firstShard + i;
      std::vector<uint8_t>& d = data_[global];
      d.resize(dg::MediaShardHeader::kSize + shardBytes_);
      dg::MediaShardHeader h = MakeHeader(flags, static_cast<int>(bi), i);
      h.writeTo(d.data());
      const size_t offset = size_t(global) * shardBytes_;
      if (au && offset < auBytes) {
        const size_t bytes = std::min(size_t(shardBytes_), auBytes - offset);
        std::memcpy(d.data() + dg::MediaShardHeader::kSize, au + offset, bytes);
      }
    }
  }
}

dg::MediaShardHeader PacketizedFrame::MakeHeader(uint8_t extraFlags, int blockIdx,
                                                 int shardIndex) const {
  const Block& blk = blocks_[blockIdx];
  dg::MediaShardHeader h;
  h.flags = extraFlags;
  // WIRE.md §4.1: DEPTH_VALID is identical on data, parity and retransmissions
  // of a picture, so it belongs on every header this frame emits.
  if (desc_.depthValid) h.flags |= dg::MediaShardHeader::kDepthValid;
  h.monitorId = desc_.monitorId;
  h.sessionGen = desc_.sessionGen;
  h.layoutGen = desc_.layoutGen;
  h.codecGen = desc_.codecGen;
  h.picSeq = desc_.picSeq;
  h.contentVer = desc_.contentVer;
  h.frameBytes = frameBytes_;
  h.blockFirstShard = static_cast<uint16_t>(blk.firstShard);
  h.shardIndex = static_cast<uint16_t>(shardIndex);
  h.k = static_cast<uint8_t>(blk.k);
  h.m = static_cast<uint8_t>(blk.m);
  h.shardBytes = static_cast<uint16_t>(shardBytes_);
  h.totalDataShards = static_cast<uint16_t>(totalDataShards_);
  h.deadlineBudgetMs = desc_.deadlineBudgetMs;
  h.captureTimeUs = desc_.captureTimeUs;
  return h;
}

std::vector<std::vector<uint8_t>> PacketizedFrame::interleavedData() const {
  std::vector<std::vector<uint8_t>> out;
  out.reserve(data_.size());
  const int longest = blocks_.empty() ? 0 : blocks_.front().k;
  for (int i = 0; i < longest; ++i)
    for (const auto& block : blocks_)
      if (i < block.k) out.push_back(data_[block.firstShard + i]);
  return out;
}

std::vector<std::vector<uint8_t>> PacketizedFrame::interleavedParity() const {
  std::vector<std::vector<uint8_t>> out;
  size_t longest = 0;
  for (const auto& l : parity_) longest = longest > l.size() ? longest : l.size();
  for (size_t i = 0; i < longest; i++)
    for (const auto& l : parity_)
      if (i < l.size()) out.push_back(l[i]);
  return out;
}

void PacketizedFrame::AppendInterleavedParity(std::vector<std::vector<uint8_t>>& out) {
  size_t count = 0, longest = 0;
  for (const auto& block : parity_) {
    count += block.size();
    longest = std::max(longest, block.size());
  }
  out.reserve(out.size() + count);
  for (size_t i = 0; i < longest; ++i)
    for (auto& block : parity_)
      if (i < block.size()) out.push_back(std::move(block[i]));
  // Retain only data shards for retransmission.
  for (auto& block : parity_) block.clear();
}

void PacketizedFrame::ComputeParity() {
  if (!parity_.empty()) return;  // idempotent
  parity_.resize(blocks_.size());
  uint8_t flags = dg::MediaShardHeader::kParity;
  if (desc_.randomAccess) flags |= dg::MediaShardHeader::kRandomAccess;
  if (desc_.refinement) flags |= dg::MediaShardHeader::kRefinement;
  std::vector<const uint8_t*> dataPtrs;
  std::vector<uint8_t*> parityPtrs;
  for (size_t bi = 0; bi < blocks_.size(); ++bi) {
    const auto& block = blocks_[bi];
    if (block.m == 0) continue;
    dataPtrs.resize(block.k);
    parityPtrs.resize(block.m);
    auto& datagrams = parity_[bi];
    datagrams.resize(block.m);
    for (int i = 0; i < block.k; ++i)
      dataPtrs[i] = data_[block.firstShard + i].data() + dg::MediaShardHeader::kSize;
    for (int j = 0; j < block.m; ++j) {
      auto& datagram = datagrams[j];
      datagram.resize(dg::MediaShardHeader::kSize + shardBytes_);
      MakeHeader(flags, static_cast<int>(bi), block.k + j).writeTo(datagram.data());
      parityPtrs[j] = datagram.data() + dg::MediaShardHeader::kSize;
    }
    if (!Encode(dataPtrs.data(), block.k, parityPtrs.data(), block.m, shardBytes_))
      datagrams.clear();
  }
}

std::vector<std::vector<uint8_t>> PacketizedFrame::Retransmission(
    const std::vector<uint16_t>& indices) const {
  std::vector<std::vector<uint8_t>> out;
  if (indices.empty()) {
    out = data_;  // whole picture
  } else {
    for (uint16_t i : indices)
      if (i < totalDataShards_) out.push_back(data_[i]);
  }
  for (auto& d : out) d[1] |= dg::MediaShardHeader::kRetransmit;
  return out;
}

}  // namespace fec
}  // namespace wvd

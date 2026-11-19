// proto3 primitives, stream framing and datagram layouts (see wire1.h).
#include "wvd/wire1.h"

#include <cstring>

namespace wvd {
namespace pb {

void Writer::rawVarint(uint64_t v) {
  while (v >= 0x80) {
    bytes.push_back(static_cast<uint8_t>(v) | 0x80);
    v >>= 7;
  }
  bytes.push_back(static_cast<uint8_t>(v));
}

void Writer::s64(int field, int64_t v) {
  if (v == 0) return;
  key(field, kVarint);
  rawVarint((static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63));
}

void Writer::flt(int field, float v) {
  uint32_t bits = 0;
  std::memcpy(&bits, &v, 4);
  if (bits == 0) return;  // proto3 default
  key(field, kFixed32);
  for (int i = 0; i < 4; i++) bytes.push_back(static_cast<uint8_t>(bits >> (8 * i)));
}

void Writer::str(int field, const std::string& v) {
  if (v.empty()) return;
  key(field, kLen);
  rawVarint(v.size());
  bytes.insert(bytes.end(), v.begin(), v.end());
}

void Writer::bytesField(int field, const uint8_t* p, size_t n) {
  if (n == 0) return;
  key(field, kLen);
  rawVarint(n);
  bytes.insert(bytes.end(), p, p + n);
}

void Writer::repeatedBytes(int field, const std::vector<std::vector<uint8_t>>& all) {
  for (const auto& v : all) {  // empty elements are positional and kept
    key(field, kLen);
    rawVarint(v.size());
    bytes.insert(bytes.end(), v.begin(), v.end());
  }
}

void Writer::packedU32(int field, const std::vector<uint32_t>& v) {
  if (v.empty()) return;
  Writer inner;
  for (uint32_t x : v) inner.rawVarint(x);
  key(field, kLen);
  rawVarint(inner.bytes.size());
  bytes.insert(bytes.end(), inner.bytes.begin(), inner.bytes.end());
}

void Writer::msg(int field, const Writer& sub, bool always) {
  if (!always && sub.bytes.empty()) return;
  key(field, kLen);
  rawVarint(sub.bytes.size());
  bytes.insert(bytes.end(), sub.bytes.begin(), sub.bytes.end());
}

bool Reader::rawVarint(uint64_t& out) {
  uint64_t result = 0;
  unsigned shift = 0;
  for (;;) {
    if (i_ >= n_) return false;
    uint8_t b = p_[i_++];
    if (shift == 63 && b > 1) return false;
    result |= static_cast<uint64_t>(b & 0x7F) << shift;
    if (b < 0x80) {
      out = result;
      return true;
    }
    shift += 7;
    if (shift > 63) return false;
  }
}

bool Reader::nextField(int& field, uint8_t& wireType) {
  if (atEnd()) return false;
  uint64_t key = 0;
  if (!rawVarint(key)) return false;
  wireType = static_cast<uint8_t>(key & 7);
  if (wireType != kVarint && wireType != kFixed64 && wireType != kLen && wireType != kFixed32)
    return false;
  field = static_cast<int>(key >> 3);
  return field > 0;
}

bool Reader::s64(int64_t& out) {
  uint64_t v = 0;
  if (!rawVarint(v)) return false;
  out = static_cast<int64_t>((v >> 1) ^ (0ull - (v & 1)));  // zigzag
  return true;
}

bool Reader::flt(float& out) {
  if (i_ + 4 > n_) return false;
  uint32_t bits = static_cast<uint32_t>(p_[i_]) | (static_cast<uint32_t>(p_[i_ + 1]) << 8) |
                  (static_cast<uint32_t>(p_[i_ + 2]) << 16) |
                  (static_cast<uint32_t>(p_[i_ + 3]) << 24);
  i_ += 4;
  std::memcpy(&out, &bits, 4);
  return true;
}

bool Reader::lenDelim(const uint8_t*& p, size_t& n) {
  uint64_t len = 0;
  if (!rawVarint(len) || len > n_ - i_) return false;
  p = p_ + i_;
  n = static_cast<size_t>(len);
  i_ += n;
  return true;
}

bool Reader::str(std::string& out) {
  const uint8_t* p = nullptr;
  size_t n = 0;
  if (!lenDelim(p, n)) return false;
  out.assign(reinterpret_cast<const char*>(p), n);
  return true;
}

bool Reader::repeatedU32(uint8_t wireType, std::vector<uint32_t>& into) {
  if (wireType == kLen) {
    const uint8_t* p = nullptr;
    size_t n = 0;
    if (!lenDelim(p, n)) return false;
    Reader sub(p, n);
    uint32_t v = 0;
    while (!sub.atEnd()) {
      if (!sub.u32(v)) return false;
      into.push_back(v);
    }
    return true;
  }
  uint32_t v = 0;
  if (!u32(v)) return false;
  into.push_back(v);
  return true;
}

bool Reader::skip(uint8_t wireType) {
  switch (wireType) {
    case kVarint: {
      uint64_t v = 0;
      return rawVarint(v);
    }
    case kFixed64:
      if (i_ + 8 > n_) return false;
      i_ += 8;
      return true;
    case kFixed32:
      if (i_ + 4 > n_) return false;
      i_ += 4;
      return true;
    case kLen: {
      const uint8_t* p = nullptr;
      size_t n = 0;
      return lenDelim(p, n);
    }
    default:
      return false;
  }
}

}  // namespace pb

namespace framing {

std::vector<uint8_t> frame(const std::vector<uint8_t>& envelope) {
  std::vector<uint8_t> out;
  out.reserve(4 + envelope.size());
  uint32_t n = static_cast<uint32_t>(envelope.size());
  out.push_back(static_cast<uint8_t>(n));
  out.push_back(static_cast<uint8_t>(n >> 8));
  out.push_back(static_cast<uint8_t>(n >> 16));
  out.push_back(static_cast<uint8_t>(n >> 24));
  out.insert(out.end(), envelope.begin(), envelope.end());
  return out;
}

bool Deframer::append(const uint8_t* data, size_t len) {
  buf_.insert(buf_.end(), data, data + len);
  return !bad_;
}

bool Deframer::next(std::vector<uint8_t>& out) {
  if (bad_) return false;
  size_t avail = buf_.size() - read_;
  if (!sawPreamble_) {
    if (avail < 4) return false;
    if (std::memcmp(buf_.data() + read_, kPreamble, 4) != 0) {
      bad_ = true;
      return false;
    }
    read_ += 4;
    sawPreamble_ = true;
    avail -= 4;
  }
  if (avail < 4) return false;
  uint32_t len = static_cast<uint32_t>(buf_[read_]) | (static_cast<uint32_t>(buf_[read_ + 1]) << 8) |
                 (static_cast<uint32_t>(buf_[read_ + 2]) << 16) |
                 (static_cast<uint32_t>(buf_[read_ + 3]) << 24);
  if (len > kMaxFrameBytes) {
    bad_ = true;
    return false;
  }
  if (avail - 4 < len) return false;
  out.assign(buf_.begin() + read_ + 4, buf_.begin() + read_ + 4 + len);
  read_ += 4 + len;
  // Compact when the consumed prefix dominates (keeps appends amortised O(1)).
  if (read_ > 0 && read_ >= buf_.size() / 2) {
    buf_.erase(buf_.begin(), buf_.begin() + read_);
    read_ = 0;
  }
  return true;
}

}  // namespace framing

namespace dg {

namespace {
template <class T>
void put(void* p, size_t off, T v) {
  std::memcpy(static_cast<uint8_t*>(p) + off, &v, sizeof(v));  // LE on x86/x64
}
template <class T>
T get(const uint8_t* p, size_t off) {
  T v;
  std::memcpy(&v, p + off, sizeof(v));
  return v;
}
}  // namespace

void MediaShardHeader::writeTo(uint8_t* p) const {
  p[0] = kDgMediaShard;
  p[1] = flags;
  put(p, 2, monitorId);
  put(p, 4, sessionGen);
  put(p, 8, layoutGen);
  put(p, 12, codecGen);
  put(p, 16, picSeq);
  put(p, 20, contentVer);
  put(p, 24, frameBytes);
  put(p, 28, blockFirstShard);
  put(p, 30, shardIndex);
  p[32] = k;
  p[33] = m;
  put(p, 34, shardBytes);
  put(p, 36, totalDataShards);
  put(p, 38, deadlineBudgetMs);
  put(p, 40, captureTimeUs);
}

bool MediaShardHeader::parse(const uint8_t* p, size_t count, MediaShardHeader& o) {
  if (count < kSize || p[0] != kDgMediaShard) return false;
  o.flags = p[1];
  o.monitorId = get<uint16_t>(p, 2);
  o.sessionGen = get<uint32_t>(p, 4);
  o.layoutGen = get<uint32_t>(p, 8);
  o.codecGen = get<uint32_t>(p, 12);
  o.picSeq = get<uint32_t>(p, 16);
  o.contentVer = get<uint32_t>(p, 20);
  o.frameBytes = get<uint32_t>(p, 24);
  o.blockFirstShard = get<uint16_t>(p, 28);
  o.shardIndex = get<uint16_t>(p, 30);
  o.k = p[32];
  o.m = p[33];
  o.shardBytes = get<uint16_t>(p, 34);
  o.totalDataShards = get<uint16_t>(p, 36);
  o.deadlineBudgetMs = get<uint16_t>(p, 38);
  o.captureTimeUs = get<uint64_t>(p, 40);
  // WIRE.md §4.1 validation — never trust the network.
  if (!(o.shardBytes > 0 && o.k > 0 && o.totalDataShards > 0 &&
        static_cast<unsigned>(o.k) + o.m <= 255 && o.shardIndex < o.k + o.m &&
        static_cast<unsigned>(o.blockFirstShard) + o.k <= o.totalDataShards &&
        count - kSize >= o.shardBytes &&
        static_cast<uint64_t>(o.frameBytes) <=
            static_cast<uint64_t>(o.totalDataShards) * o.shardBytes &&
        static_cast<uint64_t>(o.frameBytes) >
            static_cast<uint64_t>(o.totalDataShards - 1) * o.shardBytes))
    return false;
  return true;
}

std::vector<uint8_t> CursorDatagram::encode(const CursorDatagram& c) {
  std::vector<uint8_t> d(kSize, 0);
  d[0] = kDgCursor;
  d[1] = c.visible ? 1 : 0;
  put(d.data(), 2, c.monitorId);
  put(d.data(), 4, c.layoutGen);
  put(d.data(), 8, c.shapeVer);
  put(d.data(), 12, c.seq);
  put(d.data(), 16, c.x);
  put(d.data(), 20, c.y);
  put(d.data(), 24, c.hostTimeUs);
  return d;
}

void AudioHeader::writeTo(uint8_t* p) const {
  p[0] = kDgAudio;
  p[1] = 1;  // signed 16-bit LE, interleaved
  p[2] = channels;
  p[3] = 0;
  put(p, 4, sessionGen);
  put(p, 8, seq);
  put(p, 12, sampleRate);
  put(p, 16, hostTimeUs);
  put(p, 24, frames);
  put(p, 26, static_cast<uint16_t>(0));
}

bool FlowHello::parse(const uint8_t* p, size_t count, FlowHello& o) {
  if (count < kSize || p[0] != kDgFlowHello) return false;
  if (get<uint32_t>(p, 4) != kMagic) return false;
  o.sessionGen = get<uint32_t>(p, 8);
  return true;
}

bool Nack::parse(const uint8_t* p, size_t count, Nack& o) {
  if (count < kHeaderSize || p[0] != kDgNack) return false;
  size_t n = p[1];
  if (count < kHeaderSize + n * 2) return false;
  o.monitorId = get<uint16_t>(p, 2);
  o.codecGen = get<uint32_t>(p, 4);
  o.picSeq = get<uint32_t>(p, 8);
  o.missing.clear();
  for (size_t i = 0; i < n; i++) o.missing.push_back(get<uint16_t>(p, kHeaderSize + i * 2));
  return true;
}

std::vector<uint8_t> Nack::encode(const Nack& n) {
  size_t cnt = n.missing.size() > static_cast<size_t>(kMaxIndices)
                   ? static_cast<size_t>(kMaxIndices)
                   : n.missing.size();
  std::vector<uint8_t> d(kHeaderSize + cnt * 2, 0);
  d[0] = kDgNack;
  d[1] = static_cast<uint8_t>(cnt);
  put(d.data(), 2, n.monitorId);
  put(d.data(), 4, n.codecGen);
  put(d.data(), 8, n.picSeq);
  put(d.data(), 12, static_cast<uint32_t>(0));
  for (size_t i = 0; i < cnt; i++) put(d.data(), kHeaderSize + i * 2, n.missing[i]);
  return d;
}

}  // namespace dg
}  // namespace wvd

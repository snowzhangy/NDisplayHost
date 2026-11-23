// WIRE.md v1 primitives: proto3 wire-format codec, reliable-stream framing
// and the fixed little-endian datagram layouts.
//
// The normative companion is core/proto/WIRE.md and core/proto/wvd.proto; the
// byte-for-byte reference is WVDAppleCore (Wire/Proto.swift, StreamFraming.swift,
// Datagrams.swift). Hand-rolled like the Apple side so the host stays
// dependency-free; the same schema must encode identically on both ends.
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace wvd {
namespace pb {

enum WireType : uint8_t { kVarint = 0, kFixed64 = 1, kLen = 2, kFixed32 = 5 };

// proto3 writer: default values are omitted, the receiver treats absent as 0.
class Writer {
 public:
  std::vector<uint8_t> bytes;

  void rawVarint(uint64_t v);
  void key(int field, uint8_t wireType) {
    rawVarint((static_cast<uint64_t>(field) << 3) | wireType);
  }

  void u64(int field, uint64_t v) {
    if (v == 0) return;
    key(field, kVarint);
    rawVarint(v);
  }
  void u32(int field, uint32_t v) { u64(field, v); }
  void boolean(int field, bool v) {
    if (!v) return;
    key(field, kVarint);
    bytes.push_back(1);
  }
  // zigzag, for proto3 sint32/sint64 (Monitor desktop_x/desktop_y).
  void s64(int field, int64_t v);
  void s32(int field, int32_t v) { s64(field, v); }
  void flt(int field, float v);
  void str(int field, const std::string& v);
  void bytesField(int field, const uint8_t* p, size_t n);
  void bytesField(int field, const std::vector<uint8_t>& v) {
    bytesField(field, v.data(), v.size());
  }
  // Repeated bytes keep empty elements (they are positional).
  void repeatedBytes(int field, const std::vector<std::vector<uint8_t>>& all);
  // Packed repeated uint32 (proto3 default for repeated scalars).
  void packedU32(int field, const std::vector<uint32_t>& v);

  // Embedded message. `always` writes even an empty body — required for
  // oneof members, whose presence IS the message.
  void msg(int field, const Writer& sub, bool always = false);
};

class Reader {
 public:
  Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}

  bool atEnd() const { return i_ >= n_; }
  bool rawVarint(uint64_t& out);
  // Next field key; false at end of message OR on malformed input. The
  // decoder must stop on error, not resynchronise.
  bool nextField(int& field, uint8_t& wireType);
  bool u64(uint64_t& out) { return rawVarint(out); }
  bool u32(uint32_t& out) {
    uint64_t v = 0;
    if (!rawVarint(v)) return false;
    out = static_cast<uint32_t>(v);
    return true;
  }
  bool boolean(bool& out) {
    uint64_t v = 0;
    if (!rawVarint(v)) return false;
    out = v != 0;
    return true;
  }
  bool s64(int64_t& out);
  bool s32(int32_t& out) {
    int64_t v = 0;
    if (!s64(v)) return false;
    out = static_cast<int32_t>(v);
    return true;
  }
  bool flt(float& out);
  bool lenDelim(const uint8_t*& p, size_t& n);
  bool str(std::string& out);
  bool bytesField(std::vector<uint8_t>& out) {
    const uint8_t* p = nullptr;
    size_t n = 0;
    if (!lenDelim(p, n)) return false;
    out.assign(p, p + n);
    return true;
  }
  // Accepts packed and unpacked repeated uint32, as parsers must.
  bool repeatedU32(uint8_t wireType, std::vector<uint32_t>& into);
  bool skip(uint8_t wireType);

 private:
  const uint8_t* p_;
  size_t n_;
  size_t i_ = 0;
};

}  // namespace pb

// WIRE.md §3: every reliable stream starts with "WVD1", then
// frame := u32 le length (<= 4 MiB) || Envelope protobuf.
namespace framing {

inline const uint8_t kPreamble[4] = {'W', 'V', 'D', '1'};
constexpr uint32_t kMaxFrameBytes = 4u << 20;

std::vector<uint8_t> frame(const std::vector<uint8_t>& envelope);

// Incremental deframer: append bytes, get whole envelopes. Not thread-safe;
// owned by one connection's receive path.
class Deframer {
 public:
  // expectPreamble: the peer writes the "WVD1" preamble before its first frame.
  explicit Deframer(bool expectPreamble = true) : sawPreamble_(!expectPreamble) {}
  // Returns false on a framing violation (bad preamble / oversized frame);
  // the connection must then be dropped.
  bool append(const uint8_t* data, size_t len);
  // Pop one complete envelope body (protobuf) into `out`; false when none.
  bool next(std::vector<uint8_t>& out);
  size_t buffered() const { return buf_.size() - read_; }

 private:
  std::vector<uint8_t> buf_;
  size_t read_ = 0;
  bool sawPreamble_ = false;
  bool bad_ = false;
};

}  // namespace framing

// WIRE.md §4 datagram types.
enum DatagramType : uint8_t {
  kDgMediaShard = 0x01,
  kDgCursor = 0x02,
  kDgAudio = 0x03,
  kDgFlowHello = 0x10,
  kDgNack = 0x11,
};

namespace dg {

// Media shard header (§4.1), 48 bytes + shard_bytes payload.
struct MediaShardHeader {
  static constexpr size_t kSize = 48;
  enum Flags : uint8_t {
    kRandomAccess = 1 << 0,
    kParity = 1 << 1,
    kRefinement = 1 << 2,
    kRetransmit = 1 << 3,
    // WIRE.md §4.1: this picture's strip holds depth for this picture. Set
    // identically on data, parity and retransmissions of the picture.
    kDepthValid = 1 << 4,
  };
  uint8_t flags = 0;
  uint16_t monitorId = 0;
  uint32_t sessionGen = 0;
  uint32_t layoutGen = 0;
  uint32_t codecGen = 0;
  uint32_t picSeq = 0;
  uint32_t contentVer = 0;
  uint32_t frameBytes = 0;
  uint16_t blockFirstShard = 0;
  uint16_t shardIndex = 0;
  uint8_t k = 0;
  uint8_t m = 0;
  uint16_t shardBytes = 0;
  uint16_t totalDataShards = 0;
  uint16_t deadlineBudgetMs = 0;
  uint64_t captureTimeUs = 0;

  void writeTo(uint8_t* p) const;
  // Validates per WIRE.md; false on anything malformed.
  static bool parse(const uint8_t* p, size_t count, MediaShardHeader& out);
};

// Cursor position (§4.2), 32 bytes.
struct CursorDatagram {
  static constexpr size_t kSize = 32;
  bool visible = false;
  uint16_t monitorId = 0;
  uint32_t layoutGen = 0;
  uint32_t shapeVer = 0;
  uint32_t seq = 0;
  float x = 0.f, y = 0.f;
  uint64_t hostTimeUs = 0;

  static std::vector<uint8_t> encode(const CursorDatagram& c);
};

// Audio packet (§4.5): 28-byte header + interleaved s16 PCM.
struct AudioHeader {
  static constexpr size_t kHeaderSize = 28;
  uint8_t channels = 2;
  uint32_t sessionGen = 0;
  uint32_t seq = 0;
  uint32_t sampleRate = 48000;
  uint64_t hostTimeUs = 0;
  uint16_t frames = 0;

  void writeTo(uint8_t* p) const;  // type kDgAudio, format 1
};

// FlowHello (§4.3), 12 bytes, client -> host.
struct FlowHello {
  static constexpr size_t kSize = 12;
  static constexpr uint32_t kMagic = 0x47445657u;  // "WVDG"
  uint32_t sessionGen = 0;
  static bool parse(const uint8_t* p, size_t count, FlowHello& out);
};

// NACK (§4.4), client -> host.
struct Nack {
  static constexpr size_t kHeaderSize = 16;
  static constexpr int kMaxIndices = 255;
  uint16_t monitorId = 0;
  uint32_t codecGen = 0;
  uint32_t picSeq = 0;
  std::vector<uint16_t> missing;
  static bool parse(const uint8_t* p, size_t count, Nack& out);
  static std::vector<uint8_t> encode(const Nack& n);
};

}  // namespace dg
}  // namespace wvd

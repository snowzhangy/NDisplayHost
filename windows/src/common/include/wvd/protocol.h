// WVD wire protocol (plan §7), backend A. Versioned, little-endian, packed.
//
// Framing: PacketHeader followed by `length` payload bytes. Fixed-layout
// payload structs below; variable data (CSD, cursor pixels, media slices,
// clipboard text) trails the fixed part. This mirrors the protobuf schema the
// plan settles on for backend A; a flat packed form keeps P0 dependency-free
// while keeping the same fields and generations.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include "wvd/platform.h"  // windows headers must precede SDK headers
#include "wvd/gen.h"

namespace wvd {

inline constexpr uint32_t kWireMagic = 0x50445657;  // 'WVDP'
inline constexpr uint16_t kWireVersion = 1;

enum class PacketKind : uint16_t {
  Hello = 1,
  Capabilities = 2,
  MonitorLayout = 3,
  ModeChange = 4,           // headset -> host: ModeChangeRequest
  CodecConfig = 5,
  Input = 6,
  InputRelease = 7,
  CursorShape = 8,
  CursorPos = 9,
  Media = 10,
  Manifest = 11,           // reserved for tiles (§6.6 R1-R8), unused in P0
  RecoveryRequest = 12,
  StatsQos = 13,
  Heartbeat = 14,
  Idle = 15,
  Clipboard = 16,
  Audio = 17,              // host -> headset PCM, only after Hello asks for it
  DisplayModes = 18,        // host -> headset: DisplayModesPayload + DisplayModeDesc[]
  DepthMonitors = 19,       // headset -> host: DepthMonitorsPayload + uint16 indices
                            // (StreamSettings.depth_monitors, game-depth plan §3.1)
  // Local IPC (owner / input helper named pipes), plan §4.7-4:
  IpcCommand = 32,
  IpcResponse = 33,
};

// MPEG/HEVC VUI codes used by the §6.7 SDR reference colour contract.
enum class ColorPrimaries : uint8_t { BT709 = 1 };
enum class TransferChar : uint8_t { BT709 = 1 };  // stands in for sRGB in the VUI
enum class MatrixCoeff : uint8_t { BT709 = 1 };
enum class ChromaLoc : uint8_t { Left = 1 };  // HEVC default, §6.7

enum class Codec : uint8_t { HEVC = 0, H264 = 1, AV1 = 2 };

// §6.2 explicit pointer states. Never collapse "embedded" into "hidden"
// without positive evidence (final review #3).
enum class PointerState : uint8_t { Hidden = 0, Separate = 1, Embedded = 2 };

// §6.6 recovery kinds.
enum class RecoveryKind : uint32_t { InvalidateRefs = 0, Ltr = 1, IDR = 2 };

// Input event kinds (§4.4).
enum class InputKind : uint8_t {
  Key = 0,        // code = HID usage / scan code; flags = KEY flags below
  Button = 1,     // code = button bitmask value; x,y = desktop coords
  MoveAbs = 2,    // x,y = virtual-desktop pixels (already transformed)
  Wheel = 3,      // wheelDelta
  Touch = 4,      // contactId + x,y + pressure
  Pen = 5,
};

enum InputFlags : uint16_t {
  kKeyUp = 0x0001,
  kKeyExtend = 0x0002,      // E0 prefix
  kSystemKey = 0x0004,
  kButtonDblclk = 0x0008,
  kTouchDown = 0x0010,
  kTouchUp = 0x0020,
  kTouchMove = 0x0040,
  kTouchCancel = 0x0080,
};

enum InputReleaseReason : uint32_t {
  kReleaseDisconnect = 1,
  kReleaseDisarm = 2,
  kReleaseDesktopSwitch = 3,
  kReleaseBackendHandoff = 4,
  kReleaseIdleTimeout = 5,
};

#pragma pack(push, 1)

struct PacketHeader {
  uint32_t magic;      // kWireMagic
  uint16_t version;    // kWireVersion
  uint16_t kind;       // PacketKind
  uint32_t length;     // payload bytes following the header
  uint64_t seq;        // per-kind sequence (media: picSeq, cursor pos: seq)
};

// Hello/Capabilities (§7): decoder caps, refresh, device model, thermal state.
struct HelloPayload {
  uint32_t protocolVersion;
  uint32_t deviceModelHash;
  uint32_t maxDecodedPixelsPerSec;  // advertised by the headset
  uint32_t maxMppsAt90;
  uint8_t wantsAv1;
  uint8_t thermalState;   // nominal 0...3
  uint16_t reserved;      // bit flags, see kHello* (other bits must be 0)
  uint64_t sessionNonce;
  char deviceName[32];
};

// HelloPayload.reserved bits.
constexpr uint16_t kHelloWantsAudio = 0x0001;  // send PacketKind::Audio
constexpr uint16_t kHelloEmbeddedDepth =
    0x0002;  // peer DecoderCaps.embedded_depth: it can unpack a coded picture
             // with a depth strip (capability only — never a request, §3.3)

// PacketKind::Audio body: interleaved little-endian int16 samples follow
// (frames * channels). Sent only to a headset that set kHelloWantsAudio.
struct AudioPayload {
  uint64_t sessionGen;
  uint64_t seq;          // +1 per packet
  int64_t qpcCapture;
  uint32_t sampleRate;   // device mix rate (the headset resamples)
  uint16_t channels;     // 2
  uint16_t format;       // 1 = s16le interleaved
  uint32_t frames;       // <= 2048
  uint32_t reserved;
};

// §6.7 colour contract. All streams start with the SDR reference values.
struct ColorVui {
  uint8_t primaries;    // ColorPrimaries
  uint8_t transfer;     // TransferChar
  uint8_t matrix;       // MatrixCoeff
  uint8_t fullRange;    // video_full_range_flag
  uint8_t chromaLeft;   // chroma_sample_loc_type = left
  uint8_t reserved[3];
};
inline constexpr ColorVui kSdrReferenceVui{
    static_cast<uint8_t>(ColorPrimaries::BT709),
    static_cast<uint8_t>(TransferChar::BT709),
    static_cast<uint8_t>(MatrixCoeff::BT709), 1,
    static_cast<uint8_t>(ChromaLoc::Left), {0, 0, 0}};

// Monitor entry: id is the CCD/GDI device path (utf8, truncated if long).
struct MonitorDesc {
  char id[64];
  int32_t x, y;         // virtual-desktop coordinates (may be negative)
  uint32_t width, height;
  uint32_t dpiX, dpiY;
  uint32_t refreshMilliHz;
  uint8_t primary;
  uint8_t rotation;     // 0/90/180/270
  uint16_t reserved;
};

struct MonitorLayoutPayload {
  uint64_t layoutGen;
  uint32_t count;       // MonitorDesc[count] follows
  uint32_t captureIndex;
};

// CodecConfig{codec_gen, codec, profile, size, colour VUI} — sent reliably on
// the control stream before any media of that generation (§6.6).
// CodecConfig: width/height are the CODED size — with an embedded depth strip
// the strip and its guard band are inside `height` (game-depth plan §3.3).
struct CodecConfigPayload {
  uint64_t codecGen;
  uint8_t codec;        // Codec
  uint8_t hevcProfile;  // e.g. 1 = Main, 2 = Main10
  uint8_t flags;        // bit0 intraRefresh, bit1 sliceOutput, bit2 ltr
  uint8_t reserved;     // bit 0: embedded depth, bits 1–3: stereo layout
  uint32_t width, height;
  uint32_t fpsNum, fpsDen;
  uint32_t bitrateKbps;
  ColorVui vui;
  uint32_t csdSize;     // parameter sets trail the struct (after the layout when present)
};

// CodecConfigPayload.reserved bits. The three stereo bits carry msg::StereoLayout.
constexpr uint8_t kCodecConfigStereoShift = 1;
constexpr uint8_t kCodecConfigStereoMask = 0x0e;
constexpr uint8_t kCodecConfigEmbeddedDepth = 0x01;  // EmbeddedDepthPayload follows this struct,
                                                     // parameter sets follow the layout

// Input (§7): Input{input_gen, seq} + events.
// The headset sends pointer events in MONITOR SURFACE pixels + monitor
// index; the media worker transforms them to virtual-desktop coordinates
// (§4.4: per-monitor DPI, rotation, negative origins) before injection.
struct InputEvent {
  uint8_t kind;         // InputKind
  uint8_t flags;        // InputFlags
  uint16_t code;        // key: HID usage; button: MOUSEEVENTF_*-style bit
  int32_t x, y;         // key/wheel: 0; pointer: monitor pixels (headset) or
                        // desktop coords (after worker transform)
  uint16_t monitor;     // monitor index for pointer events
  uint16_t reserved;
  uint32_t contactId;   // touch/pen
  int32_t wheelDelta;
};

struct InputPayload {
  uint64_t inputGen;
  uint64_t seq;
  uint32_t count;       // InputEvent[count] follows
};

// Display size (the headset's size menu). The host lists the modes the
// streamed monitor supports; the headset asks for one; the host applies it
// for the session and answers with a fresh list (plus, when the size
// changed, a new MonitorLayout and CodecConfig through the normal resize
// path). Changes last for the session: the host restores the monitor's
// configured mode when the session ends.
enum class ModeChangeStatus : uint32_t {
  None = 0,         // list sent without a request (session start, size change)
  Applied = 1,      // the monitor now runs the requested mode
  Unchanged = 2,    // it already did
  Failed = 3,       // the display stack refused the mode
  Unsupported = 4,  // not in the list / not a monitor this session streams
};

enum DisplayModeFlags : uint32_t {
  kModeCurrent = 1,          // the monitor's mode right now
  kModeNative = 2,           // the monitor's preferred (native) mode
  kModeScaledForEncode = 4,  // larger than the encode pixel budget: scaled down
};

struct ModeChangeRequest {
  uint32_t requestId;       // echoed in DisplayModesPayload::requestId
  uint16_t monitor;         // index into the current MonitorLayout
  uint16_t reserved;
  uint32_t width, height;   // one of the listed modes
  uint32_t refreshMilliHz;  // 0 = the host picks (keeps the current rate if it can)
};

struct DisplayModesPayload {
  uint64_t layoutGen;       // the layout this list belongs to
  uint16_t monitor;         // index into that layout
  uint16_t count;           // DisplayModeDesc entries that follow
  uint32_t requestId;       // the request this list answers (0 = none)
  uint32_t status;          // ModeChangeStatus of that request
  uint32_t reserved;
};

struct DisplayModeDesc {
  uint32_t width, height;
  uint32_t refreshMilliHz;
  uint32_t flags;           // DisplayModeFlags
};

struct InputReleasePayload {
  uint64_t inputGen;    // generation whose state must be released
  uint32_t reason;      // InputReleaseReason
  uint32_t reserved;
};

// Cursor (§6.2): position datagram carries shape version + layout gen.
struct CursorPosPayload {
  uint64_t seq;
  int64_t tsQpc;        // PC QPC at sample time
  uint32_t shapeVer;
  uint64_t layoutGen;
  int32_t x, y;         // monitor pixels of `monitor`
  uint16_t monitor;     // index into current layout
  uint8_t state;        // PointerState
  uint8_t reserved;
};

struct CursorShapePayload {
  uint32_t shapeVer;
  int32_t hotX, hotY;
  uint32_t width, height;
  uint32_t pixelFormat; // 0 = BGRA8 premul, 1 = color-keyed (mono/cursor color)
  uint32_t dataSize;    // pixels trail the struct
  uint32_t colorKey;    // BGRA when pixelFormat==1
};

// Media{session_gen, layout_gen, codec_gen, pic_seq, content_ver, deadline,
//        monitor, slice, fec} (§7). P0: one unit per picture (sliceCount=1).
struct MediaPayloadHeader {
  uint64_t sessionGen, layoutGen, codecGen;
  uint32_t picSeq, contentVer;
  int64_t deadlineQpc;  // absolute presentation deadline (PC clock domain)
  uint16_t monitor, slice, sliceCount;
  uint16_t flags;       // bit0 keyframe, bit1 refinement, bit2 lastSlice, bit3 depthValid
  uint32_t dataSize;    // AU bytes trail the struct
  uint32_t videoEpoch;
};

// MediaPayloadHeader.flags bit3: this picture carries a valid depth strip
// (maps to WIRE.md §4.1 flag bit4 DEPTH_VALID; the QUIC adapter maps it).
constexpr uint16_t kMediaDepthValid = 1u << 3;

// StreamSettings.depth_monitors for the pipeline (indices into MonitorLayout,
// 0-based; the headset's 1-based monitor ids are mapped by the QUIC adapter).
struct DepthMonitorsPayload {
  uint32_t count;   // uint16 indices follow
  uint32_t reserved;
};

struct RecoveryRequestPayload {
  uint64_t codecGen;
  uint32_t kind;        // RecoveryKind
  uint32_t refPicSeq;
};

// Per-stage timestamps (§8), single PC clock domain (QPC).
struct StageTimings {
  int64_t captureQpc;
  int64_t copyQpc;
  int64_t encodeInQpc;
  int64_t encodeOutQpc;
  int64_t sendQpc;
};

struct StatsQosPayload {
  uint64_t sessionGen;
  uint64_t picSeq;
  StageTimings t;
  uint32_t capQueueDepth, encQueueDepth, sendQueueDepth;
  uint32_t droppedFrames, missedUpdates, unexpectedMissedUpdates;
  uint32_t encodeKbps, framesPerSec;
};

struct HeartbeatPayload {
  uint64_t sessionGen;
  int64_t pcQpc;
};

struct ClipboardPayload {
  uint32_t format;      // 0 = utf8 text
  uint32_t dataLen;     // bytes trail the struct
};

// Local IPC over named pipes (§4.7-4). One IpcCommand packet per call.
enum IpcCmd : uint32_t {
  CmdPing = 1,
  // owner:
  OwnerGetStatus = 10,
  OwnerSessionStart = 11,
  OwnerSessionStop = 12,
  OwnerRestoreTopology = 13,
  OwnerSetKeepAwake = 14,   // flags: 1 on / 0 off (both power requests, §4.2)
  // input helper (§4.4/§4.7):
  InputArm = 20,            // consent granted for device, flags=1
  InputDisarm = 21,
  InputInject = 22,         // payload: InputPayload + events (already validated)
  InputReleaseAll = 23,     // InputReleasePayload
  InputGetState = 24,
};

struct IpcHeader {
  uint32_t cmd;         // IpcCmd
  uint32_t flags;
  uint64_t callerAuth;  // per-message authentication token (§4.7-4)
};

struct IpcStatusResponse {
  uint32_t state;       // process-specific state enum
  uint32_t detail;
  uint64_t sessionGen;
  uint64_t layoutGen;
  uint64_t inputGen;
};
// Input helper status: state 0=disarmed, 1=armed (possibly suspended).
// detail bits: 1=secure desktop, 2=rejected batch, 4=bad token, 8=suspended.
// inputGen is helper-owned and returned AFTER the command; sessionGen is held
// input count, layoutGen is a random helper instance ID (changes on restart).

#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 20, "wire header");
static_assert(sizeof(MediaPayloadHeader) == 56, "wire media header");
static_assert(sizeof(HelloPayload) == 60, "hello layout");
static_assert(sizeof(MonitorDesc) == 96, "monitor layout");
static_assert(sizeof(CodecConfigPayload) == 44, "codec layout");
static_assert(sizeof(InputEvent) == 24, "input layout");
static_assert(sizeof(CursorShapePayload) == 32, "cursor shape layout");
static_assert(sizeof(CursorPosPayload) == 40, "cursor position layout");
static_assert(sizeof(AudioPayload) == 40, "audio layout");
static_assert(sizeof(ModeChangeRequest) == 20, "mode change layout");
static_assert(sizeof(DisplayModesPayload) == 24, "display modes layout");
static_assert(sizeof(DisplayModeDesc) == 16, "display mode entry layout");

// ---- builder / view ---------------------------------------------------------

class PacketWriter {
 public:
  PacketWriter(PacketKind kind, uint64_t seq = 0) {
    hdr_.magic = kWireMagic;
    hdr_.version = kWireVersion;
    hdr_.kind = static_cast<uint16_t>(kind);
    hdr_.length = 0;
    hdr_.seq = seq;
  }

  template <class T>
  void Append(const T& pod) {
    const auto* b = reinterpret_cast<const uint8_t*>(&pod);
    buf_.insert(buf_.end(), b, b + sizeof(T));
    hdr_.length += static_cast<uint32_t>(sizeof(T));
  }

  void AppendBytes(const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    buf_.insert(buf_.end(), b, b + n);
    hdr_.length += static_cast<uint32_t>(n);
  }

  template <class T>
  void AppendArray(const std::vector<T>& v) {
    if (!v.empty()) AppendBytes(v.data(), v.size() * sizeof(T));
  }

  std::vector<uint8_t> Take() {
    std::vector<uint8_t> out;
    out.reserve(sizeof(PacketHeader) + buf_.size());
    const auto* h = reinterpret_cast<const uint8_t*>(&hdr_);
    out.insert(out.end(), h, h + sizeof(PacketHeader));
    out.insert(out.end(), buf_.begin(), buf_.end());
    return out;
  }

  size_t size() const { return sizeof(PacketHeader) + buf_.size(); }

 private:
  PacketHeader hdr_;
  std::vector<uint8_t> buf_;
};

// Zero-copy view over one received frame (including header).
class PacketView {
 public:
  PacketView() = default;

  // buf must contain a complete packet (header.length bytes after header).
  static std::optional<PacketView> Parse(const uint8_t* buf, size_t len) {
    if (len < sizeof(PacketHeader)) return std::nullopt;
    PacketHeader h;
    std::memcpy(&h, buf, sizeof(h));
    if (h.magic != kWireMagic || h.version != kWireVersion) return std::nullopt;
    if (len < sizeof(PacketHeader) + h.length) return std::nullopt;
    PacketView v;
    v.hdr_ = h;
    v.payload_ = buf + sizeof(PacketHeader);
    return v;
  }

  const PacketHeader& header() const { return hdr_; }
  PacketKind kind() const { return static_cast<PacketKind>(hdr_.kind); }
  const uint8_t* payload() const { return payload_; }
  uint32_t payloadSize() const { return hdr_.length; }

  // Unaligned-safe typed read of one POD at a payload offset.
  template <class T>
  std::optional<T> Get(uint32_t offset = 0) const {
    if (offset + sizeof(T) > hdr_.length) return std::nullopt;
    T v{};
    std::memcpy(&v, payload_ + offset, sizeof(T));
    return v;
  }

  // POD array read (e.g. MonitorDesc[] after MonitorLayoutPayload).
  template <class T>
  std::vector<T> ArrayAt(uint32_t offset, uint32_t count) const {
    std::vector<T> out;
    if (offset > hdr_.length || count > (hdr_.length - offset) / sizeof(T)) return out;
    out.resize(count);
    if (count) std::memcpy(out.data(), payload_ + offset, size_t(count) * sizeof(T));
    return out;
  }

  const uint8_t* VarAt(uint32_t offset) const {
    return offset >= hdr_.length ? nullptr : payload_ + offset;
  }

 private:
  PacketHeader hdr_{};
  const uint8_t* payload_ = nullptr;
};

}  // namespace wvd

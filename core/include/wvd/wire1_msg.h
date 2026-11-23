// wvd.proto v1 message structs + hand-rolled protobuf codec (host side).
// Field numbers and defaults follow proto/wvd.proto exactly; the reference
// codec is WVDAppleCore Wire/Messages.swift. Only the messages a host needs
// are modelled; unknown fields are skipped on read.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wvd/wire1.h"

namespace wvd {
namespace msg {

enum class Codec : uint32_t { Unknown = 0, Hevc = 1, H264 = 2, Av1 = 3 };
enum class StereoLayout : uint32_t {
  None = 0, SideBySide = 1, SideBySideHalf = 2, TopBottom = 3, TopBottomHalf = 4,
};
enum class ThermalState : uint32_t { Nominal = 0, Fair = 1, Serious = 2, Critical = 3 };
enum class AuthResult : uint32_t { Ok = 0, PairingRequired = 1, PairingStarted = 2,
                                   Rejected = 3, LockedOut = 4, Busy = 5 };
enum class InputArmState : uint32_t { Disarmed = 0, ConsentPending = 1, Armed = 2, Denied = 3 };
enum class CursorState : uint32_t { Separate = 0, Embedded = 1, Hidden = 2 };
enum class RecoveryKind : uint32_t { Idr = 0, Invalidate = 1, Ltr = 2 };
enum class MouseButton : uint32_t { Left = 0, Right = 1, Middle = 2 };
enum class ModeChangeStatus : uint32_t { None = 0, Applied = 1, Unchanged = 2, Failed = 3,
                                         Unsupported = 4 };

struct DecoderCaps {
  bool hevc = false, hevcMain10 = false, h264 = false, av1 = false;
  uint32_t maxWidth = 0, maxHeight = 0;
  uint64_t maxPixelRate = 0;
  // Field 8: the client can unpack a picture with an embedded depth strip.
  bool embeddedDepth = false;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

struct Hello {
  uint32_t protocolVersion = 0;
  std::string deviceId, deviceName, deviceModel;
  std::vector<uint8_t> devicePublicKey;  // P-256 X9.63 uncompressed (65 B)
  std::vector<uint8_t> authSignature;    // ECDSA P-256/SHA-256 DER
  bool requestPairing = false;
  DecoderCaps decoder;
  std::vector<uint32_t> displayRefreshMhz;
  ThermalState thermal = ThermalState::Nominal;
  uint32_t maxDatagramSize = 0;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

struct HelloAck {
  uint32_t protocolVersion = 1;
  std::string hostId, hostName, hostOs;
  AuthResult auth = AuthResult::Ok;
  uint32_t sessionGen = 0;
  uint64_t hostTimeUs = 0;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

struct PairingChallenge {
  uint32_t sasDigits = 6, timeoutMs = 120000;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

struct PairingConfirm {
  bool accepted = false;
  std::vector<uint8_t> deviceSignature;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

struct PairingResult {
  bool success = false;
  std::string reason;
  std::vector<uint8_t> hostCertSha256;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

struct SessionState {
  InputArmState input = InputArmState::Disarmed;
  uint32_t inputGen = 0;
  bool workstationLocked = false;
  std::string message;
  bool mediaFlowBound = false;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

struct Monitor {
  uint32_t monitorId = 0;
  std::string name;
  uint32_t width = 0, height = 0, refreshMhz = 0, ppi = 0;
  int32_t desktopX = 0, desktopY = 0;
  bool primary = false;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

struct MonitorLayout {
  uint32_t layoutGen = 0;
  std::vector<Monitor> monitors;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

struct ColorInfo {
  uint32_t primaries = 1, transfer = 13, matrix = 1;
  bool fullRange = true;
  uint32_t chromaLocation = 0;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

// Embedded depth strip (wvd.proto EmbeddedDepthLayout, WIRE.md §6.1). The
// depth map is a grayscale strip drawn under the colour in the same coded
// picture; presence on a generation is CodecConfig.embeddedDepth.
enum class DepthEncoding : uint32_t { InverseNearWhite = 0 };

struct EmbeddedDepthLayout {
  uint32_t pictureWidth = 0, pictureHeight = 0;  // colour rect at (0,0)
  uint32_t stripY = 0;
  uint32_t mapWidth = 0, mapHeight = 0;
  uint32_t slices = 0;
  DepthEncoding encoding = DepthEncoding::InverseNearWhite;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

struct CodecConfig {
  uint32_t monitorId = 0, codecGen = 0;
  Codec codec = Codec::Hevc;
  uint32_t width = 0, height = 0;  // CODED size (depth strip included)
  std::vector<std::vector<uint8_t>> parameterSets;  // no start codes
  ColorInfo color;
  uint32_t nalLengthSize = 4;
  uint32_t frameRateMhz = 0;
  bool hasEmbeddedDepth = false;  // proto3 message presence
  EmbeddedDepthLayout embeddedDepth;
  StereoLayout stereoLayout = StereoLayout::None;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

struct CursorShapeMsg {
  uint32_t monitorId = 0, shapeVer = 0, layoutGen = 0;
  uint32_t width = 0, height = 0, hotX = 0, hotY = 0;
  std::vector<uint8_t> rgba;
  CursorState state = CursorState::Separate;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

struct RecoveryRequest {
  uint32_t monitorId = 0, codecGen = 0;
  RecoveryKind kind = RecoveryKind::Idr;
  uint32_t lastGoodPicSeq = 0, lostPicSeq = 0;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

struct Ping {
  uint64_t clientSendUs = 0;
  uint32_t seq = 0;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

struct Pong {
  uint64_t clientSendUs = 0, hostRecvUs = 0, hostSendUs = 0;
  uint32_t seq = 0;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

struct QueueDepth {
  std::string name;
  uint32_t depth = 0, capacity = 0, peak = 0;
  uint64_t dropped = 0;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

struct ClientStats {
  uint32_t monitorId = 0;
  uint64_t windowUs = 0;
  uint32_t framesReceived = 0, framesDecoded = 0, framesPresented = 0, framesLate = 0;
  uint32_t framesLost = 0;
  uint32_t shardsReceived = 0, shardsMissing = 0, shardsRecovered = 0;
  uint64_t bytesReceived = 0;
  uint32_t decodeUsP50 = 0, decodeUsP95 = 0, e2eUsP50 = 0, e2eUsP95 = 0, jitterUs = 0;
  std::vector<QueueDepth> queues;
  int64_t clockOffsetUs = 0;
  uint32_t rttUs = 0, nacksSent = 0, framesSuperseded = 0, framesPresentFailed = 0;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

struct HostStats {
  uint32_t monitorId = 0;
  uint32_t fpsCaptured = 0, fpsEncoded = 0, bitrateKbps = 0;
  uint32_t encodeUsP50 = 0, encodeUsP95 = 0;
  std::vector<QueueDepth> queues;
  uint32_t targetBitrateKbps = 0, retransmittedShards = 0, idrCount = 0;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

struct Visibility {
  bool visible = false;
  std::vector<uint32_t> monitorIds;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

struct ThermalUpdate {
  ThermalState state = ThermalState::Nominal;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

struct InputRelease {
  uint32_t inputGen = 0;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

struct InputStreamOpen {
  uint32_t sessionGen = 0;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

struct StreamSettings {
  uint32_t maxBitrateKbps = 0, frameRate = 0;
  bool preferQuality = false;
  bool audio = false;
  // Monitors whose viewer wants game depth now (3D on). Empty = none. Every
  // StreamSettings carries the full current list.
  std::vector<uint32_t> depthMonitors;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

struct Goodbye {
  std::string reason;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

struct PointerMove {
  uint32_t monitorId = 0, layoutGen = 0;
  float x = 0.f, y = 0.f;
};
struct PointerButtonMsg {
  uint32_t monitorId = 0, layoutGen = 0, clickCount = 0;
  float x = 0.f, y = 0.f;
  MouseButton button = MouseButton::Left;
  bool down = false;
};
struct ScrollMsg {
  float dx = 0.f, dy = 0.f;
  bool precise = false;
};
struct KeyMsg {
  uint32_t hidUsage = 0;
  bool down = false;
};
struct TextMsg {
  std::string text;
};

struct InputEvent {
  uint32_t seq = 0;
  uint64_t clientTimeUs = 0;
  enum class Kind : uint8_t { None, Move, Button, Scroll, Key, Text } kind = Kind::None;
  PointerMove move;
  PointerButtonMsg button;
  ScrollMsg scroll;
  KeyMsg key;
  TextMsg text;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

struct InputEvents {
  uint32_t inputGen = 0;
  std::vector<InputEvent> events;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

struct DisplayMode {
  uint32_t width = 0, height = 0, refreshMhz = 0;
  bool current = false, native = false, scaledForEncode = false;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

struct DisplayModes {
  uint32_t monitorId = 0, layoutGen = 0;
  std::vector<DisplayMode> modes;
  uint32_t requestId = 0;
  ModeChangeStatus status = ModeChangeStatus::None;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

struct DisplayModeRequest {
  uint32_t monitorId = 0, width = 0, height = 0, refreshMhz = 0, requestId = 0;
  void write(pb::Writer& w) const;
  bool read(pb::Reader& r);
};

// Envelope (proto oneof `msg`). Exactly one member is set.
struct Envelope {
  enum class Kind : uint8_t {
    None, Hello, HelloAck, PairingChallenge, PairingConfirm, PairingResult, SessionState,
    MonitorLayout, CodecConfig, CursorShape, RecoveryRequest, Ping, Pong, ClientStats, HostStats,
    Visibility, ThermalUpdate, InputEvents, InputRelease, StreamSettings, Goodbye,
    InputStreamOpen, DisplayModes, DisplayModeRequest,
  };
  Kind kind = Kind::None;

  // Storage for every kind; only the member matching `kind` is meaningful.
  Hello hello;
  HelloAck helloAck;
  PairingChallenge pairingChallenge;
  PairingConfirm pairingConfirm;
  PairingResult pairingResult;
  SessionState sessionState;
  MonitorLayout monitorLayout;
  CodecConfig codecConfig;
  CursorShapeMsg cursorShape;
  RecoveryRequest recoveryRequest;
  Ping ping;
  Pong pong;
  ClientStats clientStats;
  HostStats hostStats;
  Visibility visibility;
  ThermalUpdate thermal;
  InputEvents input;
  InputRelease inputRelease;
  StreamSettings streamSettings;
  Goodbye goodbye;
  InputStreamOpen inputStreamOpen;
  DisplayModes displayModes;
  DisplayModeRequest displayModeRequest;

  std::vector<uint8_t> encode() const;
  // False on malformed protobuf (the stream is then dropped).
  bool decode(const uint8_t* p, size_t n);
};

}  // namespace msg
}  // namespace wvd

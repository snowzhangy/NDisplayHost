// wvd.proto v1 message codec (see wire1_msg.h). Mechanical, one block per
// message; field numbers follow proto/wvd.proto.
#include "wvd/wire1_msg.h"

namespace wvd {
namespace msg {

// Reader helpers shared by all messages: every read returns false on
// malformed input; unknown fields are skipped.
#define FIELD_LOOP()                                   \
  int f;                                               \
  uint8_t wt;                                          \
  while (r.nextField(f, wt))

// Last entry of every read switch: consume unknown fields.
#define SKIP() \
  default:     \
    if (!r.skip(wt)) return false; break;

void DecoderCaps::write(pb::Writer& w) const {
  w.boolean(1, hevc);
  w.boolean(2, hevcMain10);
  w.boolean(3, h264);
  w.boolean(4, av1);
  w.u32(5, maxWidth);
  w.u32(6, maxHeight);
  w.u64(7, maxPixelRate);
  w.boolean(8, embeddedDepth);
}
bool DecoderCaps::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.boolean(hevc)) return false; break;
      case 2: if (!r.boolean(hevcMain10)) return false; break;
      case 3: if (!r.boolean(h264)) return false; break;
      case 4: if (!r.boolean(av1)) return false; break;
      case 5: if (!r.u32(maxWidth)) return false; break;
      case 6: if (!r.u32(maxHeight)) return false; break;
      case 7: if (!r.u64(maxPixelRate)) return false; break;
      case 8: if (!r.boolean(embeddedDepth)) return false; break;
      SKIP()
    }
  }
  return true;
}

void Hello::write(pb::Writer& w) const {
  w.u32(1, protocolVersion);
  w.str(2, deviceId);
  w.str(3, deviceName);
  w.str(4, deviceModel);
  w.bytesField(5, devicePublicKey);
  w.bytesField(6, authSignature);
  w.boolean(7, requestPairing);
  { pb::Writer d; decoder.write(d); w.msg(8, d); }
  w.packedU32(9, displayRefreshMhz);
  w.u32(10, static_cast<uint32_t>(thermal));
  w.u32(11, maxDatagramSize);
}
bool Hello::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.u32(protocolVersion)) return false; break;
      case 2: if (!r.str(deviceId)) return false; break;
      case 3: if (!r.str(deviceName)) return false; break;
      case 4: if (!r.str(deviceModel)) return false; break;
      case 5: if (!r.bytesField(devicePublicKey)) return false; break;
      case 6: if (!r.bytesField(authSignature)) return false; break;
      case 7: if (!r.boolean(requestPairing)) return false; break;
      case 8: { const uint8_t* p; size_t n; if (!r.lenDelim(p, n)) return false;
                pb::Reader sub(p, n); if (!decoder.read(sub)) return false; break; }
      case 9: if (!r.repeatedU32(wt, displayRefreshMhz)) return false; break;
      case 10: { uint32_t v = 0; if (!r.u32(v)) return false; thermal = static_cast<ThermalState>(v); break; }
      case 11: if (!r.u32(maxDatagramSize)) return false; break;
      SKIP()
    }
  }
  return true;
}

void HelloAck::write(pb::Writer& w) const {
  w.u32(1, protocolVersion);
  w.str(2, hostId);
  w.str(3, hostName);
  w.str(4, hostOs);
  w.u32(5, static_cast<uint32_t>(auth));
  w.u32(6, sessionGen);
  w.u64(7, hostTimeUs);
}
bool HelloAck::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.u32(protocolVersion)) return false; break;
      case 2: if (!r.str(hostId)) return false; break;
      case 3: if (!r.str(hostName)) return false; break;
      case 4: if (!r.str(hostOs)) return false; break;
      case 5: { uint32_t v = 0; if (!r.u32(v)) return false; auth = static_cast<AuthResult>(v); break; }
      case 6: if (!r.u32(sessionGen)) return false; break;
      case 7: if (!r.u64(hostTimeUs)) return false; break;
      SKIP()
    }
  }
  return true;
}

void PairingChallenge::write(pb::Writer& w) const {
  w.u32(1, sasDigits);
  w.u32(2, timeoutMs);
}
bool PairingChallenge::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.u32(sasDigits)) return false; break;
      case 2: if (!r.u32(timeoutMs)) return false; break;
      SKIP()
    }
  }
  return true;
}

void PairingConfirm::write(pb::Writer& w) const {
  w.boolean(1, accepted);
  w.bytesField(2, deviceSignature);
}
bool PairingConfirm::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.boolean(accepted)) return false; break;
      case 2: if (!r.bytesField(deviceSignature)) return false; break;
      SKIP()
    }
  }
  return true;
}

void PairingResult::write(pb::Writer& w) const {
  w.boolean(1, success);
  w.str(2, reason);
  w.bytesField(3, hostCertSha256);
}
bool PairingResult::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.boolean(success)) return false; break;
      case 2: if (!r.str(reason)) return false; break;
      case 3: if (!r.bytesField(hostCertSha256)) return false; break;
      SKIP()
    }
  }
  return true;
}

void SessionState::write(pb::Writer& w) const {
  w.u32(1, static_cast<uint32_t>(input));
  w.u32(2, inputGen);
  w.boolean(3, workstationLocked);
  w.str(4, message);
  w.boolean(5, mediaFlowBound);
}
bool SessionState::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: { uint32_t v = 0; if (!r.u32(v)) return false; input = static_cast<InputArmState>(v); break; }
      case 2: if (!r.u32(inputGen)) return false; break;
      case 3: if (!r.boolean(workstationLocked)) return false; break;
      case 4: if (!r.str(message)) return false; break;
      case 5: if (!r.boolean(mediaFlowBound)) return false; break;
      SKIP()
    }
  }
  return true;
}

void Monitor::write(pb::Writer& w) const {
  w.u32(1, monitorId);
  w.str(2, name);
  w.u32(3, width);
  w.u32(4, height);
  w.u32(5, refreshMhz);
  w.u32(6, ppi);
  w.s32(7, desktopX);
  w.s32(8, desktopY);
  w.boolean(9, primary);
}
bool Monitor::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.u32(monitorId)) return false; break;
      case 2: if (!r.str(name)) return false; break;
      case 3: if (!r.u32(width)) return false; break;
      case 4: if (!r.u32(height)) return false; break;
      case 5: if (!r.u32(refreshMhz)) return false; break;
      case 6: if (!r.u32(ppi)) return false; break;
      case 7: if (!r.s32(desktopX)) return false; break;
      case 8: if (!r.s32(desktopY)) return false; break;
      case 9: if (!r.boolean(primary)) return false; break;
      SKIP()
    }
  }
  return true;
}

void MonitorLayout::write(pb::Writer& w) const {
  w.u32(1, layoutGen);
  for (const auto& m : monitors) {
    pb::Writer sub;
    m.write(sub);
    w.msg(2, sub, true);
  }
}
bool MonitorLayout::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.u32(layoutGen)) return false; break;
      case 2: { const uint8_t* p; size_t n; if (!r.lenDelim(p, n)) return false;
                monitors.emplace_back();
                pb::Reader sub(p, n);
                if (!monitors.back().read(sub)) return false; break; }
      SKIP()
    }
  }
  return true;
}

void ColorInfo::write(pb::Writer& w) const {
  w.u32(1, primaries);
  w.u32(2, transfer);
  w.u32(3, matrix);
  w.boolean(4, fullRange);
  w.u32(5, chromaLocation);
}
bool ColorInfo::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.u32(primaries)) return false; break;
      case 2: if (!r.u32(transfer)) return false; break;
      case 3: if (!r.u32(matrix)) return false; break;
      case 4: if (!r.boolean(fullRange)) return false; break;
      case 5: if (!r.u32(chromaLocation)) return false; break;
      SKIP()
    }
  }
  return true;
}

void EmbeddedDepthLayout::write(pb::Writer& w) const {
  w.u32(1, pictureWidth);
  w.u32(2, pictureHeight);
  w.u32(3, stripY);
  w.u32(4, mapWidth);
  w.u32(5, mapHeight);
  w.u32(6, slices);
  w.u32(7, static_cast<uint32_t>(encoding));
}
bool EmbeddedDepthLayout::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.u32(pictureWidth)) return false; break;
      case 2: if (!r.u32(pictureHeight)) return false; break;
      case 3: if (!r.u32(stripY)) return false; break;
      case 4: if (!r.u32(mapWidth)) return false; break;
      case 5: if (!r.u32(mapHeight)) return false; break;
      case 6: if (!r.u32(slices)) return false; break;
      case 7: { uint32_t v = 0; if (!r.u32(v)) return false;
                encoding = static_cast<DepthEncoding>(v); break; }
      SKIP()
    }
  }
  return true;
}

void CodecConfig::write(pb::Writer& w) const {
  w.u32(1, monitorId);
  w.u32(2, codecGen);
  w.u32(3, static_cast<uint32_t>(codec));
  w.u32(4, width);
  w.u32(5, height);
  w.repeatedBytes(6, parameterSets);
  { pb::Writer c; color.write(c); w.msg(7, c); }
  w.u32(8, nalLengthSize);
  w.u32(9, frameRateMhz);
  if (hasEmbeddedDepth) {
    pb::Writer d; embeddedDepth.write(d); w.msg(10, d, true);
  } else {
    w.u32(11, static_cast<uint32_t>(stereoLayout));
  }
}
bool CodecConfig::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.u32(monitorId)) return false; break;
      case 2: if (!r.u32(codecGen)) return false; break;
      case 3: { uint32_t v = 0; if (!r.u32(v)) return false; codec = static_cast<Codec>(v); break; }
      case 4: if (!r.u32(width)) return false; break;
      case 5: if (!r.u32(height)) return false; break;
      case 6: if (!r.bytesField(parameterSets.emplace_back())) return false; break;
      case 7: { const uint8_t* p; size_t n; if (!r.lenDelim(p, n)) return false;
                pb::Reader sub(p, n); if (!color.read(sub)) return false; break; }
      case 8: if (!r.u32(nalLengthSize)) return false; break;
      case 9: if (!r.u32(frameRateMhz)) return false; break;
      case 10: { const uint8_t* p; size_t n; if (!r.lenDelim(p, n)) return false;
                 pb::Reader sub(p, n);
                 hasEmbeddedDepth = true;
                 if (!embeddedDepth.read(sub)) return false; break; }
      case 11: {
        uint32_t v = 0;
        if (!r.u32(v)) return false;
        stereoLayout = v <= uint32_t(StereoLayout::TopBottomHalf)
                           ? static_cast<StereoLayout>(v) : StereoLayout::None;
        break;
      }
      SKIP()
    }
  }
  if (hasEmbeddedDepth) stereoLayout = StereoLayout::None;
  return true;
}

void CursorShapeMsg::write(pb::Writer& w) const {
  w.u32(1, monitorId);
  w.u32(2, shapeVer);
  w.u32(3, layoutGen);
  w.u32(4, width);
  w.u32(5, height);
  w.u32(6, hotX);
  w.u32(7, hotY);
  w.bytesField(8, rgba);
  w.u32(9, static_cast<uint32_t>(state));
}
bool CursorShapeMsg::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.u32(monitorId)) return false; break;
      case 2: if (!r.u32(shapeVer)) return false; break;
      case 3: if (!r.u32(layoutGen)) return false; break;
      case 4: if (!r.u32(width)) return false; break;
      case 5: if (!r.u32(height)) return false; break;
      case 6: if (!r.u32(hotX)) return false; break;
      case 7: if (!r.u32(hotY)) return false; break;
      case 8: if (!r.bytesField(rgba)) return false; break;
      case 9: { uint32_t v = 0; if (!r.u32(v)) return false; state = static_cast<CursorState>(v); break; }
      SKIP()
    }
  }
  return true;
}

void RecoveryRequest::write(pb::Writer& w) const {
  w.u32(1, monitorId);
  w.u32(2, codecGen);
  w.u32(3, static_cast<uint32_t>(kind));
  w.u32(4, lastGoodPicSeq);
  w.u32(5, lostPicSeq);
}
bool RecoveryRequest::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.u32(monitorId)) return false; break;
      case 2: if (!r.u32(codecGen)) return false; break;
      case 3: { uint32_t v = 0; if (!r.u32(v)) return false; kind = static_cast<RecoveryKind>(v); break; }
      case 4: if (!r.u32(lastGoodPicSeq)) return false; break;
      case 5: if (!r.u32(lostPicSeq)) return false; break;
      SKIP()
    }
  }
  return true;
}

void Ping::write(pb::Writer& w) const {
  w.u64(1, clientSendUs);
  w.u32(2, seq);
}
bool Ping::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.u64(clientSendUs)) return false; break;
      case 2: if (!r.u32(seq)) return false; break;
      SKIP()
    }
  }
  return true;
}

void Pong::write(pb::Writer& w) const {
  w.u64(1, clientSendUs);
  w.u64(2, hostRecvUs);
  w.u64(3, hostSendUs);
  w.u32(4, seq);
}
bool Pong::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.u64(clientSendUs)) return false; break;
      case 2: if (!r.u64(hostRecvUs)) return false; break;
      case 3: if (!r.u64(hostSendUs)) return false; break;
      case 4: if (!r.u32(seq)) return false; break;
      SKIP()
    }
  }
  return true;
}

void QueueDepth::write(pb::Writer& w) const {
  w.str(1, name);
  w.u32(2, depth);
  w.u32(3, capacity);
  w.u32(4, peak);
  w.u64(5, dropped);
}
bool QueueDepth::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.str(name)) return false; break;
      case 2: if (!r.u32(depth)) return false; break;
      case 3: if (!r.u32(capacity)) return false; break;
      case 4: if (!r.u32(peak)) return false; break;
      case 5: if (!r.u64(dropped)) return false; break;
      SKIP()
    }
  }
  return true;
}

// ClientStats/HostStats share the QueueDepth read pattern.
static bool ReadQueues(pb::Reader& r, std::vector<QueueDepth>& into) {
  const uint8_t* p;
  size_t n;
  if (!r.lenDelim(p, n)) return false;
  pb::Reader sub(p, n);
  into.emplace_back();
  return into.back().read(sub);
}

void ClientStats::write(pb::Writer& w) const {
  w.u32(1, monitorId);
  w.u64(2, windowUs);
  w.u32(3, framesReceived);
  w.u32(4, framesDecoded);
  w.u32(5, framesPresented);
  w.u32(6, framesLate);
  w.u32(7, framesLost);
  w.u32(8, shardsReceived);
  w.u32(9, shardsMissing);
  w.u32(10, shardsRecovered);
  w.u64(11, bytesReceived);
  w.u32(12, decodeUsP50);
  w.u32(13, decodeUsP95);
  w.u32(14, e2eUsP50);
  w.u32(15, e2eUsP95);
  w.u32(16, jitterUs);
  for (const auto& q : queues) { pb::Writer s; q.write(s); w.msg(17, s, true); }
  w.s64(18, clockOffsetUs);
  w.u32(19, rttUs);
  w.u32(20, nacksSent);
  w.u32(21, framesSuperseded);
  w.u32(22, framesPresentFailed);
}
bool ClientStats::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.u32(monitorId)) return false; break;
      case 2: if (!r.u64(windowUs)) return false; break;
      case 3: if (!r.u32(framesReceived)) return false; break;
      case 4: if (!r.u32(framesDecoded)) return false; break;
      case 5: if (!r.u32(framesPresented)) return false; break;
      case 6: if (!r.u32(framesLate)) return false; break;
      case 7: if (!r.u32(framesLost)) return false; break;
      case 8: if (!r.u32(shardsReceived)) return false; break;
      case 9: if (!r.u32(shardsMissing)) return false; break;
      case 10: if (!r.u32(shardsRecovered)) return false; break;
      case 11: if (!r.u64(bytesReceived)) return false; break;
      case 12: if (!r.u32(decodeUsP50)) return false; break;
      case 13: if (!r.u32(decodeUsP95)) return false; break;
      case 14: if (!r.u32(e2eUsP50)) return false; break;
      case 15: if (!r.u32(e2eUsP95)) return false; break;
      case 16: if (!r.u32(jitterUs)) return false; break;
      case 17: if (!ReadQueues(r, queues)) return false; break;
      case 18: if (!r.s64(clockOffsetUs)) return false; break;
      case 19: if (!r.u32(rttUs)) return false; break;
      case 20: if (!r.u32(nacksSent)) return false; break;
      case 21: if (!r.u32(framesSuperseded)) return false; break;
      case 22: if (!r.u32(framesPresentFailed)) return false; break;
      SKIP()
    }
  }
  return true;
}

void HostStats::write(pb::Writer& w) const {
  w.u32(1, monitorId);
  w.u32(2, fpsCaptured);
  w.u32(3, fpsEncoded);
  w.u32(4, bitrateKbps);
  w.u32(5, encodeUsP50);
  w.u32(6, encodeUsP95);
  for (const auto& q : queues) { pb::Writer s; q.write(s); w.msg(7, s, true); }
  w.u32(8, targetBitrateKbps);
  w.u32(9, retransmittedShards);
  w.u32(10, idrCount);
}
bool HostStats::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.u32(monitorId)) return false; break;
      case 2: if (!r.u32(fpsCaptured)) return false; break;
      case 3: if (!r.u32(fpsEncoded)) return false; break;
      case 4: if (!r.u32(bitrateKbps)) return false; break;
      case 5: if (!r.u32(encodeUsP50)) return false; break;
      case 6: if (!r.u32(encodeUsP95)) return false; break;
      case 7: if (!ReadQueues(r, queues)) return false; break;
      case 8: if (!r.u32(targetBitrateKbps)) return false; break;
      case 9: if (!r.u32(retransmittedShards)) return false; break;
      case 10: if (!r.u32(idrCount)) return false; break;
      SKIP()
    }
  }
  return true;
}

void Visibility::write(pb::Writer& w) const {
  w.boolean(1, visible);
  w.packedU32(2, monitorIds);
}
bool Visibility::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.boolean(visible)) return false; break;
      case 2: if (!r.repeatedU32(wt, monitorIds)) return false; break;
      SKIP()
    }
  }
  return true;
}

void ThermalUpdate::write(pb::Writer& w) const {
  w.u32(1, static_cast<uint32_t>(state));
}
bool ThermalUpdate::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: { uint32_t v = 0; if (!r.u32(v)) return false; state = static_cast<ThermalState>(v); break; }
      SKIP()
    }
  }
  return true;
}

void InputRelease::write(pb::Writer& w) const { w.u32(1, inputGen); }
bool InputRelease::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.u32(inputGen)) return false; break;
      SKIP()
    }
  }
  return true;
}

void InputStreamOpen::write(pb::Writer& w) const { w.u32(1, sessionGen); }
bool InputStreamOpen::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.u32(sessionGen)) return false; break;
      SKIP()
    }
  }
  return true;
}

void StreamSettings::write(pb::Writer& w) const {
  w.u32(1, maxBitrateKbps);
  w.u32(2, frameRate);
  w.boolean(3, preferQuality);
  w.boolean(4, audio);
  w.packedU32(5, depthMonitors);
}
bool StreamSettings::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.u32(maxBitrateKbps)) return false; break;
      case 2: if (!r.u32(frameRate)) return false; break;
      case 3: if (!r.boolean(preferQuality)) return false; break;
      case 4: if (!r.boolean(audio)) return false; break;
      case 5: if (!r.repeatedU32(wt, depthMonitors)) return false; break;
      SKIP()
    }
  }
  return true;
}

void Goodbye::write(pb::Writer& w) const { w.str(1, reason); }
bool Goodbye::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.str(reason)) return false; break;
      SKIP()
    }
  }
  return true;
}

// PointerMove/PointerButton/Scroll/Key/Text: nested inside InputEvent's oneof.
static void WriteMove(pb::Writer& w, const PointerMove& m) {
  w.u32(1, m.monitorId);
  w.u32(2, m.layoutGen);
  w.flt(3, m.x);
  w.flt(4, m.y);
}
static bool ReadMove(pb::Reader& r, PointerMove& m) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.u32(m.monitorId)) return false; break;
      case 2: if (!r.u32(m.layoutGen)) return false; break;
      case 3: if (!r.flt(m.x)) return false; break;
      case 4: if (!r.flt(m.y)) return false; break;
      SKIP()
    }
  }
  return true;
}
static void WriteButton(pb::Writer& w, const PointerButtonMsg& b) {
  w.u32(1, b.monitorId);
  w.u32(2, b.layoutGen);
  w.flt(3, b.x);
  w.flt(4, b.y);
  w.u32(5, static_cast<uint32_t>(b.button));
  w.boolean(6, b.down);
  w.u32(7, b.clickCount);
}
static bool ReadButton(pb::Reader& r, PointerButtonMsg& b) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.u32(b.monitorId)) return false; break;
      case 2: if (!r.u32(b.layoutGen)) return false; break;
      case 3: if (!r.flt(b.x)) return false; break;
      case 4: if (!r.flt(b.y)) return false; break;
      case 5: { uint32_t v = 0; if (!r.u32(v)) return false; b.button = static_cast<MouseButton>(v); break; }
      case 6: if (!r.boolean(b.down)) return false; break;
      case 7: if (!r.u32(b.clickCount)) return false; break;
      SKIP()
    }
  }
  return true;
}
static void WriteScroll(pb::Writer& w, const ScrollMsg& s) {
  w.flt(1, s.dx);
  w.flt(2, s.dy);
  w.boolean(3, s.precise);
}
static bool ReadScroll(pb::Reader& r, ScrollMsg& s) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.flt(s.dx)) return false; break;
      case 2: if (!r.flt(s.dy)) return false; break;
      case 3: if (!r.boolean(s.precise)) return false; break;
      SKIP()
    }
  }
  return true;
}
static void WriteKey(pb::Writer& w, const KeyMsg& k) {
  w.u32(1, k.hidUsage);
  w.boolean(2, k.down);
}
static bool ReadKey(pb::Reader& r, KeyMsg& k) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.u32(k.hidUsage)) return false; break;
      case 2: if (!r.boolean(k.down)) return false; break;
      SKIP()
    }
  }
  return true;
}

void InputEvent::write(pb::Writer& w) const {
  w.u32(1, seq);
  w.u64(2, clientTimeUs);
  pb::Writer sub;
  switch (kind) {
    case Kind::Move: { WriteMove(sub, move); w.msg(3, sub, true); break; }
    case Kind::Button: { WriteButton(sub, button); w.msg(4, sub, true); break; }
    case Kind::Scroll: { WriteScroll(sub, scroll); w.msg(5, sub, true); break; }
    case Kind::Key: { WriteKey(sub, key); w.msg(6, sub, true); break; }
    case Kind::Text: { w.str(7, text.text); break; }
    case Kind::None: break;
  }
}
bool InputEvent::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.u32(seq)) return false; break;
      case 2: if (!r.u64(clientTimeUs)) return false; break;
      case 3: { const uint8_t* p; size_t n; if (!r.lenDelim(p, n)) return false;
                pb::Reader sub(p, n); kind = Kind::Move;
                if (!ReadMove(sub, move)) return false; break; }
      case 4: { const uint8_t* p; size_t n; if (!r.lenDelim(p, n)) return false;
                pb::Reader sub(p, n); kind = Kind::Button;
                if (!ReadButton(sub, button)) return false; break; }
      case 5: { const uint8_t* p; size_t n; if (!r.lenDelim(p, n)) return false;
                pb::Reader sub(p, n); kind = Kind::Scroll;
                if (!ReadScroll(sub, scroll)) return false; break; }
      case 6: { const uint8_t* p; size_t n; if (!r.lenDelim(p, n)) return false;
                pb::Reader sub(p, n); kind = Kind::Key;
                if (!ReadKey(sub, key)) return false; break; }
      case 7: if (!r.str(text.text)) return false; kind = Kind::Text; break;
      SKIP()
    }
  }
  return true;
}

void InputEvents::write(pb::Writer& w) const {
  w.u32(1, inputGen);
  for (const auto& e : events) { pb::Writer s; e.write(s); w.msg(2, s, true); }
}
bool InputEvents::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.u32(inputGen)) return false; break;
      case 2: { const uint8_t* p; size_t n; if (!r.lenDelim(p, n)) return false;
                pb::Reader sub(p, n);
                events.emplace_back();
                if (!events.back().read(sub)) return false; break; }
      SKIP()
    }
  }
  return true;
}

void DisplayMode::write(pb::Writer& w) const {
  w.u32(1, width);
  w.u32(2, height);
  w.u32(3, refreshMhz);
  w.boolean(4, current);
  w.boolean(5, native);
  w.boolean(6, scaledForEncode);
}
bool DisplayMode::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.u32(width)) return false; break;
      case 2: if (!r.u32(height)) return false; break;
      case 3: if (!r.u32(refreshMhz)) return false; break;
      case 4: if (!r.boolean(current)) return false; break;
      case 5: if (!r.boolean(native)) return false; break;
      case 6: if (!r.boolean(scaledForEncode)) return false; break;
      SKIP()
    }
  }
  return true;
}

void DisplayModes::write(pb::Writer& w) const {
  w.u32(1, monitorId);
  w.u32(2, layoutGen);
  for (const auto& m : modes) { pb::Writer s; m.write(s); w.msg(3, s, true); }
  w.u32(4, requestId);
  w.u32(5, static_cast<uint32_t>(status));
}
bool DisplayModes::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.u32(monitorId)) return false; break;
      case 2: if (!r.u32(layoutGen)) return false; break;
      case 3: { const uint8_t* p; size_t n; if (!r.lenDelim(p, n)) return false;
                pb::Reader sub(p, n);
                modes.emplace_back();
                if (!modes.back().read(sub)) return false; break; }
      case 4: if (!r.u32(requestId)) return false; break;
      case 5: { uint32_t v = 0; if (!r.u32(v)) return false; status = static_cast<ModeChangeStatus>(v); break; }
      SKIP()
    }
  }
  return true;
}

void DisplayModeRequest::write(pb::Writer& w) const {
  w.u32(1, monitorId);
  w.u32(2, width);
  w.u32(3, height);
  w.u32(4, refreshMhz);
  w.u32(5, requestId);
}
bool DisplayModeRequest::read(pb::Reader& r) {
  FIELD_LOOP() {
    switch (f) {
      case 1: if (!r.u32(monitorId)) return false; break;
      case 2: if (!r.u32(width)) return false; break;
      case 3: if (!r.u32(height)) return false; break;
      case 4: if (!r.u32(refreshMhz)) return false; break;
      case 5: if (!r.u32(requestId)) return false; break;
      SKIP()
    }
  }
  return true;
}

// Envelope. Oneof members are written with `always` so an empty message
// still identifies its kind.
std::vector<uint8_t> Envelope::encode() const {
  pb::Writer w;
  switch (kind) {
    case Kind::Hello: { pb::Writer s; hello.write(s); w.msg(1, s, true); break; }
    case Kind::HelloAck: { pb::Writer s; helloAck.write(s); w.msg(2, s, true); break; }
    case Kind::PairingChallenge: { pb::Writer s; pairingChallenge.write(s); w.msg(3, s, true); break; }
    case Kind::PairingConfirm: { pb::Writer s; pairingConfirm.write(s); w.msg(4, s, true); break; }
    case Kind::PairingResult: { pb::Writer s; pairingResult.write(s); w.msg(5, s, true); break; }
    case Kind::SessionState: { pb::Writer s; sessionState.write(s); w.msg(6, s, true); break; }
    case Kind::MonitorLayout: { pb::Writer s; monitorLayout.write(s); w.msg(7, s, true); break; }
    case Kind::CodecConfig: { pb::Writer s; codecConfig.write(s); w.msg(8, s, true); break; }
    case Kind::CursorShape: { pb::Writer s; cursorShape.write(s); w.msg(9, s, true); break; }
    case Kind::RecoveryRequest: { pb::Writer s; recoveryRequest.write(s); w.msg(10, s, true); break; }
    case Kind::Ping: { pb::Writer s; ping.write(s); w.msg(11, s, true); break; }
    case Kind::Pong: { pb::Writer s; pong.write(s); w.msg(12, s, true); break; }
    case Kind::ClientStats: { pb::Writer s; clientStats.write(s); w.msg(13, s, true); break; }
    case Kind::HostStats: { pb::Writer s; hostStats.write(s); w.msg(14, s, true); break; }
    case Kind::Visibility: { pb::Writer s; visibility.write(s); w.msg(15, s, true); break; }
    case Kind::ThermalUpdate: { pb::Writer s; thermal.write(s); w.msg(16, s, true); break; }
    case Kind::InputEvents: { pb::Writer s; input.write(s); w.msg(17, s, true); break; }
    case Kind::InputRelease: { pb::Writer s; inputRelease.write(s); w.msg(18, s, true); break; }
    case Kind::StreamSettings: { pb::Writer s; streamSettings.write(s); w.msg(19, s, true); break; }
    case Kind::Goodbye: { pb::Writer s; goodbye.write(s); w.msg(20, s, true); break; }
    case Kind::InputStreamOpen: { pb::Writer s; inputStreamOpen.write(s); w.msg(21, s, true); break; }
    case Kind::DisplayModes: { pb::Writer s; displayModes.write(s); w.msg(22, s, true); break; }
    case Kind::DisplayModeRequest: { pb::Writer s; displayModeRequest.write(s); w.msg(23, s, true); break; }
    case Kind::None: break;
  }
  return w.bytes;
}

bool Envelope::decode(const uint8_t* p, size_t n) {
  pb::Reader r(p, n);
  int f;
  uint8_t wt;
  while (r.nextField(f, wt)) {
    if (wt != pb::kLen) {
      if (!r.skip(wt)) return false;
      continue;
    }
    const uint8_t* sp;
    size_t sn;
    if (!r.lenDelim(sp, sn)) return false;
    pb::Reader sub(sp, sn);
    Kind k = static_cast<Kind>(f);
    if (k < Kind::Hello || k > Kind::DisplayModeRequest) continue;  // unknown member
    bool ok = true;
    switch (k) {
      case Kind::Hello: ok = hello.read(sub); break;
      case Kind::HelloAck: ok = helloAck.read(sub); break;
      case Kind::PairingChallenge: ok = pairingChallenge.read(sub); break;
      case Kind::PairingConfirm: ok = pairingConfirm.read(sub); break;
      case Kind::PairingResult: ok = pairingResult.read(sub); break;
      case Kind::SessionState: ok = sessionState.read(sub); break;
      case Kind::MonitorLayout: ok = monitorLayout.read(sub); break;
      case Kind::CodecConfig: ok = codecConfig.read(sub); break;
      case Kind::CursorShape: ok = cursorShape.read(sub); break;
      case Kind::RecoveryRequest: ok = recoveryRequest.read(sub); break;
      case Kind::Ping: ok = ping.read(sub); break;
      case Kind::Pong: ok = pong.read(sub); break;
      case Kind::ClientStats: ok = clientStats.read(sub); break;
      case Kind::HostStats: ok = hostStats.read(sub); break;
      case Kind::Visibility: ok = visibility.read(sub); break;
      case Kind::ThermalUpdate: ok = thermal.read(sub); break;
      case Kind::InputEvents: ok = input.read(sub); break;
      case Kind::InputRelease: ok = inputRelease.read(sub); break;
      case Kind::StreamSettings: ok = streamSettings.read(sub); break;
      case Kind::Goodbye: ok = goodbye.read(sub); break;
      case Kind::InputStreamOpen: ok = inputStreamOpen.read(sub); break;
      case Kind::DisplayModes: ok = displayModes.read(sub); break;
      case Kind::DisplayModeRequest: ok = displayModeRequest.read(sub); break;
      default: break;
    }
    if (!ok) return false;
    kind = k;
    return true;  // oneof: exactly one member
  }
  return kind != Kind::None;
}

}  // namespace msg
}  // namespace wvd

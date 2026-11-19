#include "cursor/cursor_source.h"

#include "wvd/logging.h"
#include "wvd/time.h"

namespace wvd {

bool CursorSource::CursorShowingFlag() {
  CURSORINFO ci{};
  ci.cbSize = sizeof(ci);
  if (!GetCursorInfo(&ci)) return false;
  return (ci.flags & CURSOR_SHOWING) != 0;
}

void CursorSource::OnCapturedFrame(const CapturedFrame& f, uint16_t monitorIndex,
                                   std::vector<std::vector<uint8_t>>& out) {
  // 1. Shape: deliver on change (reliable path). shapeVer only advances on
  //    actual content change (hash).
  if (f.pointer.hasShape && f.pointer.shape.hash != lastShapeHash_) {
    lastShape_ = f.pointer.shape;
    lastShapeHash_ = f.pointer.shape.hash;
    ++shapeVer_;
    haveShape_ = true;

    PacketWriter w(PacketKind::CursorShape, shapeVer_);
    CursorShapePayload csp{};
    csp.shapeVer = shapeVer_;
    csp.hotX = int32_t(lastShape_.hotX);
    csp.hotY = int32_t(lastShape_.hotY);
    csp.width = lastShape_.width;
    csp.height = lastShape_.height;
    csp.pixelFormat = lastShape_.pixelFormat;
    csp.colorKey = lastShape_.colorKey;
    csp.dataSize = uint32_t(lastShape_.pixels.size());
    w.Append(csp);
    w.AppendBytes(lastShape_.pixels.data(), lastShape_.pixels.size());
    out.push_back(w.Take());
    WVD_LOGI("cursor shape v%u (%ux%u hot %d,%d fmt %u)", shapeVer_,
             lastShape_.width, lastShape_.height, lastShape_.hotX, lastShape_.hotY,
             lastShape_.pixelFormat);
  }

  // 2. State decision (§6.2 table, Path D column).
  //
  // DDA PointerPosition is only meaningful when LastMouseUpdateTime is
  // non-zero; on frames where the mouse did not move the flag is stale and
  // reads FALSE. Trusting it flipped a perfectly visible pointer into
  // Embedded/Hidden, which means no overlay cursor at all. So visibility is
  // latched and only refreshed on real pointer updates.
  if (f.pointer.positionUpdated) {
    pointerVisible_ = f.pointer.visible;
    havePointerInfo_ = true;
  } else if (!havePointerInfo_) {
    // Nothing from the source yet (mouse never moved since we started): seed
    // from the input desktop so the very first frame can show a cursor.
    pointerVisible_ = CursorShowingFlag();
    havePointerInfo_ = true;
  }

  PointerState newState;
  if (pointerVisible_) {
    newState = PointerState::Separate;
  } else {
    // Visible == FALSE on a real pointer update: embedded (OS drew it into the
    // frame) vs hidden (app hid it). Evidence: CURSOR_SHOWING on the desktop.
    newState = CursorShowingFlag() ? PointerState::Embedded : PointerState::Hidden;
  }

  if (sourceJustChanged_) {
    // No trust in the fresh source's "hidden" yet: keep overlay from last
    // valid shape+position (double cursor beats missing cursor).
    if (newState == PointerState::Hidden && (havePos_ || haveShape_)) {
      newState = PointerState::Separate;
    }
    sourceJustChanged_ = false;
  }
  const PointerState prevState = state_;
  state_ = newState;

  // 3. Position. Screen coords → monitor-relative for the headset.
  //    The session knows the monitor's desktop origin; store screen coords
  //    in the packet and let the receiver map via layoutGen (layout carries
  //    origins) — matches "Every position update carries shape version +
  //    monitor generation".
  if (f.pointer.positionUpdated) {
    lastX_ = f.pointer.x;
    lastY_ = f.pointer.y;
    havePos_ = true;
  }

  // A state CHANGE must reach the headset even if it means "clear the overlay"
  // — otherwise it keeps drawing a cursor that is no longer there, or never
  // draws one that is.
  const bool stateChanged = state_ != prevState;

  if (stateChanged && state_ == PointerState::Hidden) {
    ++posSeq_;
    PacketWriter w(PacketKind::CursorPos, posSeq_);
    CursorPosPayload cp{};
    cp.seq = posSeq_;
    cp.tsQpc = f.qpcCapture;
    cp.shapeVer = shapeVer_;
    cp.layoutGen = gens_.layoutGen.load();
    cp.x = havePos_ ? lastX_ : 0;
    cp.y = havePos_ ? lastY_ : 0;
    cp.monitor = monitorIndex;
    cp.state = uint8_t(PointerState::Hidden);
    w.Append(cp);
    out.push_back(w.Take());
    return;
  }

  if (state_ == PointerState::Separate && havePos_) {
    ++posSeq_;
    PacketWriter w(PacketKind::CursorPos, posSeq_);
    CursorPosPayload cp{};
    cp.seq = posSeq_;
    cp.tsQpc = f.qpcCapture;
    cp.shapeVer = shapeVer_;
    cp.layoutGen = gens_.layoutGen.load();
    cp.x = lastX_;
    cp.y = lastY_;
    cp.monitor = monitorIndex;
    cp.state = uint8_t(state_);
    w.Append(cp);
    out.push_back(w.Take());
  } else if (state_ == PointerState::Embedded) {
    // Notify the headset that the video contains the pointer (stop overlay).
    ++posSeq_;
    PacketWriter w(PacketKind::CursorPos, posSeq_);
    CursorPosPayload cp{};
    cp.seq = posSeq_;
    cp.tsQpc = f.qpcCapture;
    cp.shapeVer = shapeVer_;
    cp.layoutGen = gens_.layoutGen.load();
    cp.x = havePos_ ? lastX_ : 0;
    cp.y = havePos_ ? lastY_ : 0;
    cp.monitor = monitorIndex;
    cp.state = uint8_t(state_);
    w.Append(cp);
    out.push_back(w.Take());
  }
  // Hidden: no overlay, no packet (the state transition itself is silent by
  // design; the headset clears after the last datagram was consumed).
}

}  // namespace wvd

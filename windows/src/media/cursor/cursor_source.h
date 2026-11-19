// Cursor source — three explicit pointer states, shape versioning, and the
// "never hide the only cursor" rule (§6.2, final review #3).
//
// Rules implemented:
//  * Separate: DDA PointerPosition.Visible == TRUE → app must draw the
//    overlay. Position datagrams are sequence-numbered on every move and
//    carry {shapeVer, layoutGen}.
//  * Visible == FALSE alone cannot distinguish Embedded from Hidden.
//    Positive evidence of embedding required (GetCursorInfo's CURSOR_SHOWING
//    on the input desktop — flag only, never position).
//  * After any source change (DDA re-creation, desktop switch, mode change)
//    we keep drawing the overlay from the last valid shape + position until
//    a frame gives explicit state. A temporary double cursor is preferred
//    over a missing one.
//  * GetCursorInfo is NEVER used for position (DPI-virtualisation breakage).
#pragma once

#include <vector>

#include "capture/dda_capture.h"
#include "wvd/gen.h"
#include "wvd/protocol.h"

namespace wvd {

class CursorSource {
 public:
  explicit CursorSource(GenCounters& gens) : gens_(gens) {}

  // Call after (re-)creating the capture source: enters the fallback state
  // and forces shape re-delivery ("the shape is re-initialised after any
  // source change", §6.2).
  void NoteSourceRecreated() {
    sourceJustChanged_ = true;
    lastShapeHash_ = 0;
    havePointerInfo_ = false;  // new source: re-seed visibility
  }

  // Process one captured frame's pointer info; emits wire packets (CursorShape
  // on shape change, CursorPos every frame while state != Hidden).
  void OnCapturedFrame(const CapturedFrame& f, uint16_t monitorIndex,
                       std::vector<std::vector<uint8_t>>& outPackets);

  // Layout changed: CursorPos carries layoutGen; nothing else to reset.
  // (gens_.layoutGen is bumped by the session.)

  PointerState lastState() const { return state_; }
  uint32_t shapeVersion() const { return shapeVer_; }

 private:
  bool CursorShowingFlag();  // CURSOR_SHOWING on current input desktop

  GenCounters& gens_;
  PointerState state_ = PointerState::Hidden;
  bool sourceJustChanged_ = true;

  uint32_t shapeVer_ = 0;
  uint64_t lastShapeHash_ = 0;
  bool haveShape_ = false;
  CursorShapeData lastShape_;

  int32_t lastX_ = 0, lastY_ = 0;  // last valid position (§6.2 fallback)
  bool havePos_ = false;
  uint64_t posSeq_ = 0;

  // Latched pointer visibility. DDA only reports it reliably on frames where
  // the mouse actually moved (LastMouseUpdateTime != 0); on other frames the
  // field is stale and must not be trusted.
  bool pointerVisible_ = false;
  bool havePointerInfo_ = false;
};

}  // namespace wvd

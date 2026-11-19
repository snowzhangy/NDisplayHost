// Path D capture: Desktop Duplication of a monitor's output (plan §4.1).
//
// Contract implemented here:
//  * The D3D device is created on the output's adapter.
//  * P0 correctness rule (final review #1): every desktop image update is a
//    FULL-FRAME copy into the next ring slot; slots are fully initialised at
//    allocation. No partial dirty-rect copies. A frame that only moved the
//    pointer carries no new image and is not copied.
//  * The OS-owned frame is released as soon as the copy is queued
//    (CopyResource); we never hold it past ReleaseFrame.
//  * Pointer metadata is reported in the three explicit states (§6.2):
//    Separate (Visible=TRUE), Embedded / Hidden (Visible=FALSE, disambiguated
//    by evidence in CursorSource, not here).
//  * DXGI_ERROR_ACCESS_LOST (desktop switch, mode change, UAC) marks the
//    session dead; the caller re-creates (generation bump is the caller's).
#pragma once

#include <wrl/client.h>

#include <vector>

#include "dx.h"
#include "wvd/gen.h"
#include "wvd/protocol.h"

namespace wvd {

// Pointer shape converted to BGRA8 (premultiplied where alpha exists).
struct CursorShapeData {
  uint32_t width = 0, height = 0;
  int32_t hotX = 0, hotY = 0;
  uint32_t pixelFormat = 0;  // 0 = BGRA premul, 1 = color-keyed
  uint32_t colorKey = 0;
  std::vector<uint8_t> pixels;  // width*height*4
  uint64_t hash = 0;            // content hash → shape version
};

// Pointer information extracted from one DDA frame.
struct CapturePointerInfo {
  bool visible = false;         // DDA PointerPosition.Visible → Separate
  bool positionUpdated = false; // LastMouseUpdateTime moved this frame
  int32_t x = 0, y = 0;         // screen coordinates of the pointer position
  bool hasShape = false;        // shape buffer delivered with this frame
  CursorShapeData shape;
};

struct CapturedFrame {
  uint64_t contentVer = 0;      // bumped only when dirty rects are present
  bool hadDirtyRects = false;
  std::vector<RECT> dirtyRects;
  std::vector<DXGI_OUTDUPL_MOVE_RECT> moves;
  int64_t qpcCapture = 0;
  int64_t qpcCopyDone = 0;
  CapturePointerInfo pointer;
  bool copied = false;          // a full-frame copy was made into ringSlot
  uint32_t ringSlot = 0;        // slot holding the full-frame copy (if copied)
};

// Ring of full-frame BGRA slots, GPU-side (§4.1 "copy it into a ring texture
// we own"). Single producer (capture), single consumer (encode), newest-wins.
class FrameRing {
 public:
  HRESULT Init(ID3D11Device* dev, UINT width, UINT height, UINT slotCount = 3);
  void Deinit();
  UINT width() const { return width_; }
  UINT height() const { return height_; }

  // Producer side.
  UINT AcquireWriteSlot();  // rotating; returns index for CopyFullFrame
  ID3D11Texture2D* SlotTexture(UINT i) { return slots_[i].Get(); }
  ID3D11ShaderResourceView* SlotSrv(UINT i) { return srvs_[i].Get(); }
  void Publish(UINT idx, uint64_t seq);
  // Re-offer the newest published slot to the consumer: refinement and
  // recovery on a still desktop re-encode the retained frame (§6.4). False if
  // nothing was published yet.
  bool RepublishNewest(uint64_t seq);

  // Full-frame CPU upload into a slot (used by the synthetic source and by
  // tests). Still a FULL frame (§4.1 copy-correctness rule).
  void WriteCpuFrame(UINT idx, const uint8_t* bgra, UINT srcPitch);
  ID3D11Device* Device() { return device_.Get(); }

  // Consumer side: newest published slot (newest-wins, §6.6).
  bool AcquireLatest(UINT& idxOut, uint64_t& seqOut);
  void Release(UINT idx) { (void)idx; }  // P0: slots are overwritten on write

  // Slot lifetime guard. A mode change makes the capture thread re-allocate
  // the slots; the encode thread may be holding a raw SRV from the old ones.
  // Init()/Deinit() take this exclusively; the consumer holds it shared for as
  // long as it uses a texture/SRV obtained from the ring.
  void LockShared() { AcquireSRWLockShared(&lifetime_); }
  void UnlockShared() { ReleaseSRWLockShared(&lifetime_); }
  struct SharedGuard {
    explicit SharedGuard(FrameRing& r) : r_(r) { r_.LockShared(); }
    ~SharedGuard() { r_.UnlockShared(); }
    FrameRing& r_;
  };

  struct Stats {
    uint64_t written, consumed, overwrittenUnconsumed;
    size_t depth;
  };
  Stats GetStats() const;

 private:
  HRESULT InitLocked(ID3D11Device* dev, UINT width, UINT height, UINT slotCount);
  UINT width_ = 0, height_ = 0, slots_32_ = 0;
  ComPtr<ID3D11Device> device_;
  std::vector<ComPtr<ID3D11Texture2D>> slots_;
  std::vector<ComPtr<ID3D11ShaderResourceView>> srvs_;
  SRWLOCK lock_ = SRWLOCK_INIT;
  // Guards slot ALLOCATION against consumer use (mode change rebuilds slots
  // while the encode thread may hold a raw SRV from the previous set).
  SRWLOCK lifetime_ = SRWLOCK_INIT;
  UINT nextWrite_ = 0;
  // Ring bookkeeping: seq per slot (0 = never written).
  std::vector<uint64_t> slotSeq_;
  UINT newestIdx_ = 0;
  uint64_t newestSeq_ = 0;
  bool hasNewest_ = false;
  uint64_t written_ = 0, consumed_ = 0, overwritten_ = 0;
};

// Desktop duplication wrapper.
class DdaCapture {
 public:
  struct InitDesc {
    ComPtr<IDXGIOutput> output;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    FrameRing* ring;      // full-frame copies land here (caller-owned)
  };

  HRESULT Start(const InitDesc& d);
  void Stop();
  bool active() const { return !!dup_; }

  // AUTHORITATIVE source size, in physical pixels, taken from the duplication
  // itself (never from desktop coordinates — those are DPI-scaled unless the
  // process is per-monitor DPI aware). The ring must be sized to this.
  UINT width() const { return desc_.ModeDesc.Width; }
  UINT height() const { return desc_.ModeDesc.Height; }
  UINT refreshHz() const {
    UINT den = desc_.ModeDesc.RefreshRate.Denominator;
    return den ? desc_.ModeDesc.RefreshRate.Numerator / den : 60;
  }
  DXGI_MODE_ROTATION rotation() const { return desc_.Rotation; }
  void SetRing(FrameRing* r) { ring_ = r; }

  // One-shot desktop grab of this output into a ring slot.
  //
  // Desktop Duplication only reports CHANGES: on a completely idle desktop
  // AcquireNextFrame keeps returning WAIT_TIMEOUT and never hands over the
  // current image. A peer that connects then sees nothing at all. So we
  // bootstrap the first picture with GDI (documented P0 fallback; the steady
  // state still comes from DDA).
  bool CaptureBootstrapFrame(UINT& slotOut);

  // Waits up to timeoutMs for a new desktop frame. Returns:
  //   S_OK            — frame metadata in `out`; the image is in the ring when
  //                     out.copied is set
  //   DXGI_ERROR_WAIT_TIMEOUT — no desktop change (static content)
  //   DXGI_ERROR_ACCESS_LOST  — re-create required (§4.1)
  // The image is copied when the desktop picture changed, or always when
  // `needImage` is set (a full picture is owed regardless of changes).
  HRESULT Acquire(uint32_t timeoutMs, CapturedFrame& out, bool needImage = true);

 private:
  ComPtr<IDXGIOutputDuplication> dup_;
  DXGI_OUTDUPL_DESC desc_{};
  RECT desktopRect_{};  // this output's rect, physical pixels (DPI-aware process)
  ComPtr<ID3D11Device> dev_;
  ComPtr<ID3D11DeviceContext> ctx_;
  FrameRing* ring_ = nullptr;
  uint64_t contentVer_ = 0;
  std::vector<uint8_t> metaBuf_;
  std::vector<uint8_t> dirtyScratch_;  // GetFrameDirtyRects output (RECTs)
  std::vector<uint8_t> moveScratch_;   // GetFrameMoveRects output
};

// Convert a DDA pointer shape (DXGI_OUTDUPL_POINTER_SHAPE_INFO + buffer) to
// BGRA8. MONOCHROME uses the classic AND/XOR approximation (§6.2 notes).
bool ConvertDdaPointerShape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& si,
                            const uint8_t* shapeBuf, size_t shapeSize,
                            CursorShapeData& out);

}  // namespace wvd

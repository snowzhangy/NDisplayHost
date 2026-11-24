#include "capture/dda_capture.h"

#include <algorithm>
#include <cstring>

#include "wvd/logging.h"
#include "wvd/time.h"

namespace wvd {

// ---------------------------------------------------------------------------
// FrameRing
// ---------------------------------------------------------------------------

HRESULT FrameRing::Init(ID3D11Device* dev, UINT width, UINT height, UINT slotCount) {
  if (!dev || !width || !height || !slotCount) return E_INVALIDARG;
  AcquireSRWLockExclusive(&lifetime_);  // excludes the encode consumer
  HRESULT hr = InitLocked(dev, width, height, slotCount);
  ReleaseSRWLockExclusive(&lifetime_);  // on EVERY path (early returns used to leak it)
  if (SUCCEEDED(hr))
    WVD_LOGI("ring: %ux%u x %u slots initialised", width, height, slotCount);
  return hr;
}

HRESULT FrameRing::InitLocked(ID3D11Device* dev, UINT width, UINT height, UINT slotCount) {
  device_ = dev;
  AcquireSRWLockExclusive(&lock_);
  width_ = width;
  height_ = height;
  // The consumer can hold one slot while a second is the newest published
  // frame. A third gives the capture thread a safe destination in either case.
  slotCount = (std::max)(3u, slotCount);
  slots_32_ = slotCount;
  slots_.assign(slotCount, nullptr);
  srvs_.assign(slotCount, nullptr);
  gameColor_.assign(slotCount, {});
  slotSeq_.assign(slotCount, 0);
  nextWrite_ = 0;
  newestIdx_ = 0;
  readingIdx_ = UINT_MAX;
  hasNewest_ = false;
  newestSeq_ = 0;
  written_ = consumed_ = overwritten_ = 0;
  ReleaseSRWLockExclusive(&lock_);

  // §4.1: every slot is fully initialised at allocation (black), so a reused
  // slot can never show uninitialised pixels.
  std::vector<uint8_t> black(size_t(width) * height * 4, 0);
  for (UINT i = 0; i < slotCount; ++i) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = 0;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = black.data();
    sd.SysMemPitch = width * 4;
    HRESULT hr = dev->CreateTexture2D(&td, &sd, &slots_[i]);
    if (FAILED(hr)) return hr;
    hr = dev->CreateShaderResourceView(slots_[i].Get(), nullptr, &srvs_[i]);
    if (FAILED(hr)) return hr;
  }
  return S_OK;
}

void FrameRing::Deinit() {
  AcquireSRWLockExclusive(&lifetime_);
  AcquireSRWLockExclusive(&lock_);
  slots_.clear();
  srvs_.clear();
  gameColor_.clear();
  slotSeq_.clear();
  slots_32_ = 0;
  hasNewest_ = false;
  newestSeq_ = 0;
  ReleaseSRWLockExclusive(&lock_);
  ReleaseSRWLockExclusive(&lifetime_);
}

UINT FrameRing::AcquireWriteSlot() {
  AcquireSRWLockExclusive(&lock_);
  UINT idx = 0;
  if (slots_32_) {
    do {
      idx = nextWrite_;
      nextWrite_ = (nextWrite_ + 1) % slots_32_;
    } while (idx == readingIdx_ || (newestSeq_ && idx == newestIdx_));
    if (slotSeq_[idx] != 0 && slotSeq_[idx] != newestSeq_) ++overwritten_;
  }
  ReleaseSRWLockExclusive(&lock_);
  return idx;
}

void FrameRing::Publish(UINT idx, uint64_t seq) {
  AcquireSRWLockExclusive(&lock_);
  if (idx < slotSeq_.size()) {
    slotSeq_[idx] = seq;
    newestIdx_ = idx;
    newestSeq_ = seq;
    hasNewest_ = true;
    ++written_;
  }
  ReleaseSRWLockExclusive(&lock_);
}

bool FrameRing::RepublishNewest(uint64_t seq) {
  AcquireSRWLockExclusive(&lock_);
  const bool ok = newestSeq_ != 0 && newestIdx_ < slotSeq_.size();
  if (ok) {
    slotSeq_[newestIdx_] = seq;
    newestSeq_ = seq;
    hasNewest_ = true;
  }
  ReleaseSRWLockExclusive(&lock_);
  return ok;
}

bool FrameRing::AcquireLatest(UINT& idxOut, uint64_t& seqOut) {
  AcquireSRWLockExclusive(&lock_);
  if (!hasNewest_ || readingIdx_ != UINT_MAX) {
    ReleaseSRWLockExclusive(&lock_);
    return false;
  }
  idxOut = newestIdx_;
  readingIdx_ = newestIdx_;
  seqOut = newestSeq_;
  hasNewest_ = false;
  ++consumed_;
  ReleaseSRWLockExclusive(&lock_);
  return true;
}

void FrameRing::Release(UINT idx) {
  AcquireSRWLockExclusive(&lock_);
  if (readingIdx_ == idx) readingIdx_ = UINT_MAX;
  ReleaseSRWLockExclusive(&lock_);
}

FrameRing::Stats FrameRing::GetStats() const {
  AcquireSRWLockShared(const_cast<SRWLOCK*>(&lock_));
  Stats s{written_, consumed_, overwritten_, 0};
  for (size_t i = 0; i < slotSeq_.size(); ++i)
    if (slotSeq_[i] != 0) ++s.depth;
  ReleaseSRWLockShared(const_cast<SRWLOCK*>(&lock_));
  return s;
}

// ---------------------------------------------------------------------------
// DdaCapture
// ---------------------------------------------------------------------------

HRESULT DdaCapture::Start(const InitDesc& d) {
  // ring may be null here: the caller sizes the FrameRing from width()/height()
  // (the authoritative physical size) and then calls SetRing().
  if (!d.output || !d.device || !d.ctx) return E_INVALIDARG;
  dev_ = d.device;
  ctx_ = d.ctx;
  ring_ = d.ring;

  ComPtr<IDXGIOutput1> out1;
  HRESULT hr = d.output->QueryInterface(IID_PPV_ARGS(&out1));
  if (FAILED(hr)) return hr;

  hr = out1->DuplicateOutput(dev_.Get(), &dup_);
  if (FAILED(hr)) {
    WVD_LOGE("DuplicateOutput failed hr=%s (needs an interactive desktop session)",
             HresultStr(hr).c_str());
    return hr;
  }
  DXGI_OUTPUT_DESC od{};
  d.output->GetDesc(&od);
  desktopRect_ = od.DesktopCoordinates;

  desc_ = {};
  dup_->GetDesc(&desc_);
  DXGI_OUTDUPL_DESC& dd = desc_;
  WVD_LOGI("DDA started: %ux%u@%u rot=%d", dd.ModeDesc.Width, dd.ModeDesc.Height,
           dd.ModeDesc.RefreshRate.Numerator /
               (dd.ModeDesc.RefreshRate.Denominator ? dd.ModeDesc.RefreshRate.Denominator : 1),
           int(dd.Rotation));
  return S_OK;
}

void DdaCapture::Stop() { dup_.Reset(); }

HRESULT DdaCapture::Acquire(uint32_t timeoutMs, CapturedFrame& out, bool needImage,
                            bool metadataOnly) {
  if (!dup_ || !ring_) return DXGI_ERROR_INVALID_CALL;

  DXGI_OUTDUPL_FRAME_INFO fi{};
  ComPtr<IDXGIResource> res;
  HRESULT hr = dup_->AcquireNextFrame(timeoutMs, &fi, &res);
  if (hr == DXGI_ERROR_WAIT_TIMEOUT) return hr;
  if (hr == DXGI_ERROR_ACCESS_LOST) {
    WVD_LOGW("DDA ACCESS_LOST (desktop switch / UAC / mode change) — re-create required");
    return hr;
  }
  if (FAILED(hr)) return hr;

  out.qpcCapture = QpcNow();
  out.contentVer = contentVer_;

  ComPtr<ID3D11Texture2D> desktop;
  hr = res.As(&desktop);
  if (FAILED(hr)) {
    dup_->ReleaseFrame();
    return hr;
  }

  // Dirty rects + moves (metadata; P0 still copies the FULL frame — §4.1
  // final review #1).
  //
  // NOTE: probing with (0, nullptr) is rejected with E_INVALIDARG — it is NOT
  // DXGI_ERROR_MORE_DATA — so the metadata must be read into a real buffer and
  // grown on MORE_DATA. The null probe silently reported "no rects" for every
  // frame, so hadDirtyRects was never true and nothing was ever encoded.
  {
    if (dirtyScratch_.empty()) dirtyScratch_.resize(64 * sizeof(RECT));
    auto* rects = reinterpret_cast<RECT*>(dirtyScratch_.data());
    UINT req = 0;
    hr = dup_->GetFrameDirtyRects(UINT(dirtyScratch_.size()), rects, &req);
    while (hr == DXGI_ERROR_MORE_DATA && req > UINT(dirtyScratch_.size())) {
      dirtyScratch_.resize(req);
      rects = reinterpret_cast<RECT*>(dirtyScratch_.data());
      hr = dup_->GetFrameDirtyRects(UINT(dirtyScratch_.size()), rects, &req);
    }
    if (SUCCEEDED(hr) && req)
      out.dirtyRects.assign(reinterpret_cast<const RECT*>(dirtyScratch_.data()),
                            reinterpret_cast<const RECT*>(dirtyScratch_.data()) +
                                req / sizeof(RECT));
  }
  {
    if (moveScratch_.empty()) moveScratch_.resize(64 * sizeof(DXGI_OUTDUPL_MOVE_RECT));
    UINT req = 0;
    hr = dup_->GetFrameMoveRects(UINT(moveScratch_.size()),
                                 reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(moveScratch_.data()),
                                 &req);
    while (hr == DXGI_ERROR_MORE_DATA && req > UINT(moveScratch_.size())) {
      moveScratch_.resize(req);
      hr = dup_->GetFrameMoveRects(
          UINT(moveScratch_.size()),
          reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(moveScratch_.data()), &req);
    }
    if (SUCCEEDED(hr) && req)
      out.moves.assign(
          reinterpret_cast<const DXGI_OUTDUPL_MOVE_RECT*>(moveScratch_.data()),
          reinterpret_cast<const DXGI_OUTDUPL_MOVE_RECT*>(moveScratch_.data()) +
              req / sizeof(DXGI_OUTDUPL_MOVE_RECT));
  }
  out.hadDirtyRects = !out.dirtyRects.empty() || !out.moves.empty();
  if (out.hadDirtyRects) {
    ++contentVer_;
    out.contentVer = contentVer_;
  }

  // Pointer shape (if delivered).
  if (fi.PointerShapeBufferSize) {
    if (metaBuf_.size() < fi.PointerShapeBufferSize)
      metaBuf_.resize(fi.PointerShapeBufferSize);
    UINT shapeReq = 0;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO si{};
    hr = dup_->GetFramePointerShape(fi.PointerShapeBufferSize, metaBuf_.data(),
                                    &shapeReq, &si);
    if (SUCCEEDED(hr) &&
        ConvertDdaPointerShape(si, metaBuf_.data(), shapeReq, out.pointer.shape)) {
      out.pointer.hasShape = true;
    }
  }

  // Pointer position + explicit visibility (§6.2).
  out.pointer.visible = fi.PointerPosition.Visible != FALSE;
  out.pointer.positionUpdated = fi.LastMouseUpdateTime.QuadPart != 0;
  out.pointer.x = fi.PointerPosition.Position.x;
  out.pointer.y = fi.PointerPosition.Position.y;

  // A pointer-only update (the mouse moved over a still desktop) reports no
  // present and no rects. Copying the whole desktop for it would cost a
  // full-frame GPU copy per mouse report for a picture nobody encodes.
  const bool imageUpdated = fi.LastPresentTime.QuadPart != 0 || fi.AccumulatedFrames != 0 ||
                            out.hadDirtyRects;
  if (metadataOnly || (!imageUpdated && !needImage)) {
    out.qpcCopyDone = out.qpcCapture;
    dup_->ReleaseFrame();
    return S_OK;
  }

  // FULL-FRAME copy into the next ring slot, then release immediately (§4.1).
  // The ring MUST be the duplication's physical size; a DPI-scaled mismatch
  // would make CopyResource fail and nothing would ever be sent.
  if (ring_->width() != desc_.ModeDesc.Width || ring_->height() != desc_.ModeDesc.Height) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      WVD_LOGE("ring is %ux%u but capture delivers %ux%u — size mismatch (DPI scaling?). "
               "Nothing will be sent.",
               ring_->width(), ring_->height(), desc_.ModeDesc.Width, desc_.ModeDesc.Height);
    }
    dup_->ReleaseFrame();
    return E_UNEXPECTED;
  }
  UINT slot = ring_->AcquireWriteSlot();
  ctx_->CopyResource(ring_->DesktopWriteTexture(slot), desktop.Get());
  out.ringSlot = slot;
  out.copied = true;
  out.qpcCopyDone = QpcNow();

  dup_->ReleaseFrame();
  return S_OK;
}

// One-shot GDI grab of this output's desktop rect into a ring slot. Used only
// to hand a peer its FIRST picture when the desktop is idle and DDA reports no
// change (§4.1: the slot still receives a FULL frame).
bool DdaCapture::CaptureBootstrapFrame(UINT& slotOut) {
  if (!ring_ || !ring_->width() || !ring_->height()) return false;
  const int w = int(ring_->width()), h = int(ring_->height());
  HDC dc = GetDC(nullptr);
  if (!dc) return false;
  HDC mem = CreateCompatibleDC(dc);
  HBITMAP bmp = mem ? CreateCompatibleBitmap(dc, w, h) : nullptr;
  bool ok = false;
  if (mem && bmp) {
    HGDIOBJ old = SelectObject(mem, bmp);
    // CAPTUREBLT so DWM-composited/layered content is included.
    if (BitBlt(mem, 0, 0, w, h, dc, desktopRect_.left, desktopRect_.top,
               SRCCOPY | CAPTUREBLT)) {
      BITMAPINFO bi{};
      bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      bi.bmiHeader.biWidth = w;
      bi.bmiHeader.biHeight = -h;  // top-down rows, matching the ring layout
      bi.bmiHeader.biPlanes = 1;
      bi.bmiHeader.biBitCount = 32;
      bi.bmiHeader.biCompression = BI_RGB;
      std::vector<uint8_t> buf(size_t(w) * size_t(h) * 4);
      if (GetDIBits(mem, bmp, 0, UINT(h), buf.data(), &bi, DIB_RGB_COLORS) == h) {
        UINT slot = ring_->AcquireWriteSlot();
        ring_->WriteCpuFrame(slot, buf.data(), UINT(w) * 4);
        slotOut = slot;
        ok = true;
      }
    }
    SelectObject(mem, old);
  }
  if (bmp) DeleteObject(bmp);
  if (mem) DeleteDC(mem);
  ReleaseDC(nullptr, dc);
  return ok;
}

// ---------------------------------------------------------------------------
// Pointer shape conversion (DXGI MONOCHROME / COLOR / MASKED_COLOR → BGRA8)
// ---------------------------------------------------------------------------

static uint64_t Fnv1a(const void* data, size_t n, uint64_t h = 1469598103934665603ull) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

bool ConvertDdaPointerShape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& si,
                            const uint8_t* buf, size_t size, CursorShapeData& out) {
  const bool mono = si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
  // DXGI counts BOTH stacked masks in Height for monochrome shapes.
  const UINT height = mono ? si.Height / 2 : si.Height;
  if (!buf || !si.Width || !height || si.Width > 512 || height > 512 ||
      (mono && (si.Height & 1)))
    return false;
  const size_t rowBytes = mono ? (si.Width + 7u) / 8u : size_t(si.Width) * 4;
  if (si.Pitch < rowBytes || size < size_t(si.Pitch) * si.Height) return false;
  const size_t pxCount = size_t(si.Width) * height;
  out.width = si.Width;
  out.height = height;
  out.hotX = si.HotSpot.x;
  out.hotY = si.HotSpot.y;
  out.pixels.assign(pxCount * 4, 0);
  out.pixelFormat = 0;
  out.colorKey = 0;

  if (mono) {
    // 1bpp AND mask followed by 1bpp XOR bitmap, rows padded to si.Pitch.
    //   and=0,xor=0 → black      and=0,xor=1 → white
    //   and=1,xor=0 → transparent and=1,xor=1 → INVERT the destination
    // We have no destination here, so invert is rendered as 50% grey: visible
    // on white AND on black, unlike the previous solid white (invisible on
    // white pages). Documented §6.2 approximation.
    //
    const size_t maskBytes = size_t(si.Pitch) * height;
    const uint8_t* andMask = buf;
    const uint8_t* xorMask = buf + maskBytes;
    for (UINT y = 0; y < height; ++y) {
      for (UINT x = 0; x < si.Width; ++x) {
        const size_t byte = size_t(y) * si.Pitch + x / 8;
        const uint8_t bit = uint8_t(0x80 >> (x % 8));
        const bool andBit = (andMask[byte] & bit) != 0;
        const bool xorBit = (xorMask[byte] & bit) != 0;
        uint8_t* dst = out.pixels.data() + (size_t(y) * si.Width + x) * 4;
        if (!andBit) {
          const uint8_t v = xorBit ? 0xFF : 0x00;  // white / black
          dst[0] = dst[1] = dst[2] = v;
          dst[3] = 0xFF;
        } else if (xorBit) {
          dst[0] = dst[1] = dst[2] = 0x80;  // invert stand-in: mid grey
          dst[3] = 0xFF;
        } else {
          dst[3] = 0;  // transparent
        }
      }
    }
  } else if (si.Type &
             (DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR |
              DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR)) {
    const bool masked = (si.Type & DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) != 0;
    for (UINT y = 0; y < si.Height; ++y) {
      const size_t row = size_t(y) * si.Pitch;
      if (row + size_t(si.Width) * 4 > size) break;  // clamp, do not reject
      const uint32_t* src = reinterpret_cast<const uint32_t*>(buf + row);
      uint32_t* dst = reinterpret_cast<uint32_t*>(out.pixels.data()) + size_t(y) * si.Width;
      for (UINT x = 0; x < si.Width; ++x) {
        const uint32_t bgra = src[x];
        if (masked) {
          // Alpha 0 means REPLACE (including opaque black); alpha 255 means
          // XOR the desktop. With no destination pixels on the cursor plane,
          // approximate nontrivial XOR with grey, as for monochrome invert.
          const uint32_t rgb = bgra & 0x00FFFFFFu;
          dst[x] = (bgra >> 24) == 0 ? (rgb | 0xFF000000u)
                                    : (rgb == 0 ? 0u : 0xFF808080u);
        } else {
          // COLOR: 32-bit ARGB with a REAL alpha channel (this is what carries
          // the drop shadow). Preserve it; forcing 0xFF turned shadows into
          // dark halos. Store premultiplied (pixelFormat 0).
          const uint32_t a = (bgra >> 24) & 0xFFu;
          if (a == 0) {
            dst[x] = 0u;
          } else if (a == 255) {
            dst[x] = bgra | 0xFF000000u;
          } else {
            const uint32_t b = ((bgra & 0xFFu) * a) / 255u;
            const uint32_t g = (((bgra >> 8) & 0xFFu) * a) / 255u;
            const uint32_t r = (((bgra >> 16) & 0xFFu) * a) / 255u;
            dst[x] = (a << 24) | (r << 16) | (g << 8) | b;
          }
        }
      }
    }
  } else {
    return false;  // HARDWARE/SOFTWARE flags carry no shape payload for us
  }

  out.hash = Fnv1a(out.pixels.data(), out.pixels.size());
  out.hash = Fnv1a(&out.width, sizeof(out.width), out.hash);
  out.hash = Fnv1a(&out.height, sizeof(out.height), out.hash);
  out.hash = Fnv1a(&si.HotSpot, sizeof(si.HotSpot), out.hash);
  return true;
}

HRESULT FrameRing::WriteGpuFrame(UINT idx, ID3D11Texture2D* source) {
  D3D11_TEXTURE2D_DESC desc{};
  source->GetDesc(&desc);
  if (desc.Width != width_ || desc.Height != height_ || desc.MipLevels != 1 ||
      desc.ArraySize != 1 || desc.SampleDesc.Count != 1)
    return E_INVALIDARG;
  ComPtr<ID3D11DeviceContext> ctx;
  device_->GetImmediateContext(&ctx);
  if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
    ctx->CopyResource(DesktopWriteTexture(idx), source);
    return S_OK;
  }
  if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM)
    return DXGI_ERROR_UNSUPPORTED;
  auto& slot = gameColor_[idx];
  if (slot.format != desc.Format) {
    GameColorSlot next;
    D3D11_TEXTURE2D_DESC local{};
    local.Width = desc.Width;
    local.Height = desc.Height;
    local.MipLevels = local.ArraySize = local.SampleDesc.Count = 1;
    local.Format = desc.Format;
    local.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device_->CreateTexture2D(&local, nullptr, &next.texture);
    if (FAILED(hr)) return hr;
    hr = device_->CreateShaderResourceView(next.texture.Get(), nullptr, &next.srv);
    if (FAILED(hr)) return hr;
    next.format = desc.Format;
    slot = std::move(next);
  }
  ctx->CopyResource(slot.texture.Get(), source);
  slot.active = true;
  return S_OK;
}

// Full-frame CPU upload into a ring slot (synthetic source / tests).
void FrameRing::WriteCpuFrame(UINT idx, const uint8_t* bgra, UINT srcPitch) {
  if (idx >= slots_.size() || !slots_[idx] || !device_) return;
  // Called from the capture (synthetic) thread; the slot vectors can be
  // re-allocated by a mode change, so take the lifetime guard.
  struct G {
    FrameRing& r;
    G(FrameRing& x) : r(x) { r.LockShared(); }
    ~G() { r.UnlockShared(); }
  } g{*this};
  ComPtr<ID3D11DeviceContext> ctx;
  device_->GetImmediateContext(&ctx);
  // The whole subresource: a D3D11_BOX left at back = 0 is EMPTY and makes
  // UpdateSubresource a silent no-op (the slot then stays black — which is
  // what the GDI bootstrap picture and the synthetic source used to show).
  ctx->UpdateSubresource(DesktopWriteTexture(idx), 0, nullptr, bgra, srcPitch, 0);
}

}  // namespace wvd

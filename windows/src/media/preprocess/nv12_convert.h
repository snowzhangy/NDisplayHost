// BGRA → NV12 preprocessing (§4.6 shared stage, §6.7 colour contract).
//
// BT.709 matrix, FULL RANGE (explicit).
//
//   Y  = 0.2126 R' + 0.7152 G' + 0.0722 B'
//   U  = (B' − Y) × 1/(2(1−0.0722)) + 0.5
//   V  = (R' − Y) × 1/(2(1−0.2126)) + 0.5
//
// The conversion renders straight into an NV12 texture (luma plane through an
// R8 view, chroma plane through an R8G8 view), so a hardware encoder on the
// same D3D11 device consumes the picture without it ever leaving the GPU.
// The same pass can scale: the session uses that to keep the encoded picture
// inside a pixel budget (Apple's MVD sizes its encoder from an 8.3 MP budget
// rather than from the desktop size).
//
// Readback() is the CPU fallback (software encoder, test encoder, an encoder
// MFT that is not D3D11-aware). Nothing is read back unless it is called.
#pragma once

#include <wrl/client.h>

#include <cstdint>
#include <vector>

#include "dx.h"

namespace wvd {

enum class ChromaFilter : uint32_t {
  Box2x2 = 0,       // standard average
  TextAware = 1,    // most saturated of 4 samples: keeps fine coloured text edges
                    // crisper at the same 4:2:0 rate (§6.4 "THE colour plan")
};

// Optional embedded-depth overlay (game-depth plan §4). The caller supplies
// the NORMALISED 8-bit inverse-depth map (or null for a black strip when
// DEPTH_VALID is clear for this frame) plus the strip geometry; the target
// texture is the full CODED size (colour + guard band + strip + padding).
struct Nv12DepthOverlay {
  const uint8_t* map = nullptr;  // mapWidth x mapHeight, row-major (null = black strip)
  UINT mapWidth = 0, mapHeight = 0;
  UINT slices = 4;
  UINT colorHeight = 0;          // rows of real colour; [colorHeight, stripY) repeats it
  UINT stripY = 0;               // first strip row in the target
  UINT sliceRows = 0;            // mapHeight / slices
};

class Nv12Converter {
 public:
  // Output textures rotate through a small pool: a picture handed to an
  // encoder stays untouched for the next kPoolSize-1 conversions, which covers
  // encoders that still read their input after the encode call returns.
  static constexpr UINT kPoolSize = 4;

  HRESULT Init(ID3D11Device* dev, ID3D11DeviceContext* ctx);

  // srcSrv: BGRA8 SRV of srcW×srcH. Produces a dstW×dstH NV12 texture (even
  // dimensions) on the same device. Scales when the sizes differ. *out is
  // owned by the converter (no reference added).
  HRESULT ConvertToTexture(ID3D11ShaderResourceView* srcSrv, UINT srcW, UINT srcH,
                           UINT dstW, UINT dstH, ChromaFilter filter,
                           ID3D11Texture2D** out);

  // With a depth overlay: dstW×dstH is the CODED size (dstH covers the guard
  // band, the strip and the black padding). Colour is written to rows
  // [0, ov.colorHeight), the guard band repeats the last colour row, the
  // strip is the sliced depth map, and strip chroma / padding stay neutral.
  HRESULT ConvertToTexture(ID3D11ShaderResourceView* srcSrv, UINT srcW, UINT srcH,
                           UINT dstW, UINT dstH, ChromaFilter filter,
                           ID3D11Texture2D** out, const Nv12DepthOverlay* ov);

  // Copies an NV12 texture from ConvertToTexture into a packed CPU buffer
  // (row pitch = width). Blocks until the GPU has finished the conversion.
  HRESULT Readback(ID3D11Texture2D* nv12, std::vector<uint8_t>& nv12Out);

  // Convenience: same-size conversion followed by a readback.
  HRESULT Convert(ID3D11ShaderResourceView* srcSrv, UINT width, UINT height,
                  ChromaFilter filter, std::vector<uint8_t>& nv12Out);

  // Packed NV12 layout helpers (row pitch = width, UV height = height/2).
  static size_t Nv12Size(UINT width, UINT height) {
    return size_t(width) * height + size_t(width) * (height / 2);
  }

 private:
  struct Target {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> luma;    // R8 view of the Y plane
    ComPtr<ID3D11RenderTargetView> chroma;  // R8G8 view of the UV plane
  };
  HRESULT EnsureTargets(UINT w, UINT h);
  HRESULT EnsureDepthTex(UINT mapW, UINT mapH);

  ComPtr<ID3D11Device> dev_;
  ComPtr<ID3D11DeviceContext> ctx_;
  ComPtr<ID3D11VertexShader> vs_;
  ComPtr<ID3D11PixelShader> psLuma_, psChroma_, psDepth_;
  ComPtr<ID3D11SamplerState> sampler_;
  ComPtr<ID3D11RasterizerState> raster_;
  ComPtr<ID3D11Buffer> cb_;
  ComPtr<ID3D11Buffer> depthCb_;
  ComPtr<ID3D11Texture2D> depthTex_;   // R8 map uploaded from the CPU
  ComPtr<ID3D11ShaderResourceView> depthSrv_;
  // Stand-ins so the PS shader-resource slots are never holes: a null in a
  // slot another pass reads wedges some drivers. Colour stand-in for the
  // depth pass, depth stand-in for the colour pass.
  ComPtr<ID3D11Texture2D> blackTex_;
  ComPtr<ID3D11ShaderResourceView> blackSrv_;
  UINT depthMapW_ = 0, depthMapH_ = 0;
  Target targets_[kPoolSize];
  UINT next_ = 0;
  UINT outW_ = 0, outH_ = 0;
  ComPtr<ID3D11Texture2D> staging_;
  UINT stagingW_ = 0, stagingH_ = 0;
  SRWLOCK lock_ = SRWLOCK_INIT;
};

}  // namespace wvd

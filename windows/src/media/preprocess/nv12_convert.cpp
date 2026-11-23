#include "preprocess/nv12_convert.h"

#include <d3d11_4.h>
#include <d3dcompiler.h>

#include <cstring>

#include "wvd/logging.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace wvd {

// Two passes over a full-screen triangle: luma into the R8 plane, chroma into
// the half-size R8G8 plane. UNORM render targets round to nearest, so no
// manual rounding is needed. Chroma filters:
//  0 = box 2×2 average
//  1 = text-aware "max chroma" — the chroma sample of the most saturated pixel
//      in the quad, preserving fine coloured text edges at 4:2:0 (§6.4).
static const char* kHlsl = R"HLSL(
cbuffer CB : register(b0) {
  float2 dstSize;   // luma size the source maps onto (colour rows; < target when depth-packed)
  uint filter;
  uint scaled;      // 0 = source and target are the same size
  uint srcHm1;      // source luma rows - 1: rows beyond the colour (guard band) clamp to it
};

// Depth-strip parameters (PS_DEPTH only).
cbuffer CBD : register(b1) {
  uint  depthMapW;    // map texels per slice
  uint  depthSliceRows;
  uint  depthBlack;   // 1 = DEPTH_VALID clear: write a black strip, sample nothing
  uint  depthStripY;  // absolute first strip row (SV_Position is viewport-space)
};

Texture2D<float4> srcTex : register(t0);
Texture2D<float>  depthTex : register(t1);   // R8_UNORM inverse depth (typed view = normalised float)
SamplerState linearClamp : register(s0);

static const float KR = 0.2126;
static const float KB = 0.0722;

float4 VS_MAIN(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

// Source colour for one target luma pixel. BGRA SRVs expose logical RGBA.
// Rows past the colour content (the 16-row guard band under the picture,
// game-depth plan §4) clamp to the last source row, which compresses to
// nearly nothing and keeps HEVC deblocking from smearing black into depth.
float3 Fetch(int2 lumaPix) {
  if (scaled != 0u)
    return srcTex.SampleLevel(linearClamp, (float2(lumaPix) + 0.5) / dstSize, 0).rgb;
  return srcTex.Load(int3(lumaPix.x, min(lumaPix.y, srcHm1), 0)).rgb;
}

float Luma(float3 c) { return KR * c.r + (1.0 - KR - KB) * c.g + KB * c.b; }

float2 Chroma(float3 c) {
  float y = Luma(c);
  return float2((c.b - y) * (1.0 / (2.0 * (1.0 - KB))) + 0.5,
                (c.r - y) * (1.0 / (2.0 * (1.0 - KR))) + 0.5);
}

float4 PS_LUMA(float4 pos : SV_Position) : SV_Target {
  return float4(Luma(Fetch(int2(pos.xy))), 0, 0, 1);
}

float4 PS_CHROMA(float4 pos : SV_Position) : SV_Target {
  int2 base = int2(pos.xy) * 2;
  float2 c00 = Chroma(Fetch(base));
  float2 c01 = Chroma(Fetch(base + int2(1, 0)));
  float2 c10 = Chroma(Fetch(base + int2(0, 1)));
  float2 c11 = Chroma(Fetch(base + int2(1, 1)));
  float2 c;
  if (filter == 1u) {
    float m00 = abs(c00.x - 0.5) + abs(c00.y - 0.5);
    float m01 = abs(c01.x - 0.5) + abs(c01.y - 0.5);
    float m10 = abs(c10.x - 0.5) + abs(c10.y - 0.5);
    float m11 = abs(c11.x - 0.5) + abs(c11.y - 0.5);
    float m = max(max(m00, m01), max(m10, m11));
    if (m == m00)      c = c00;
    else if (m == m01) c = c01;
    else if (m == m10) c = c10;
    else               c = c11;
  } else {
    c = (c00 + c01 + c10 + c11) * 0.25;
  }
  return float4(c, 0, 1);
}

// Depth strip: the map is cut into `slices` horizontal bands placed side by
// side (slice i at strip column i * map_width). The viewport covers the
// strip, so the slice seam is a pure coordinate fold.
float4 PS_DEPTH(float4 pos : SV_Position) : SV_Target {
  if (depthBlack != 0u) return 0;  // DEPTH_VALID clear: black strip
  uint slice = uint(pos.x) / depthMapW;
  uint mx = uint(pos.x) % depthMapW;
  uint my = slice * depthSliceRows + (uint(pos.y) - depthStripY);
  return float4(depthTex.Load(int3(int2(mx, my), 0)).r, 0, 0, 1);
}
)HLSL";

namespace {

// Constant buffer: naturally 32 bytes (a multiple of 16) with no padding.
struct ConvertCB {
  float dstW, dstH;
  UINT filter, scaled;
  UINT srcHm1;
  UINT pad0_, pad1_, pad2_;
};

struct DepthCB {
  UINT mapW, sliceRows, black, stripY;
};

HRESULT Compile(const char* entry, const char* target, ComPtr<ID3DBlob>& blob) {
  ComPtr<ID3DBlob> err;
  const HRESULT hr = D3DCompile(kHlsl, strlen(kHlsl), "wvd_nv12.hlsl", nullptr, nullptr, entry,
                                target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
  if (FAILED(hr))
    WVD_LOGE("NV12 shader %s compile failed: %s", entry,
             err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
  return hr;
}

// Holds the device's multithread lock for a whole state-set + draw sequence.
// The capture thread issues copies on the same immediate context; individual
// calls are serialised by the device, but a sequence is not.
struct ContextLock {
  explicit ContextLock(ID3D11DeviceContext* ctx) {
    if (ctx && SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&mt_))) && mt_) mt_->Enter();
  }
  ~ContextLock() {
    if (mt_) mt_->Leave();
  }
  ComPtr<ID3D11Multithread> mt_;
};

struct SrwExclusive {
  explicit SrwExclusive(SRWLOCK& l) : l_(l) { AcquireSRWLockExclusive(&l_); }
  ~SrwExclusive() { ReleaseSRWLockExclusive(&l_); }
  SRWLOCK& l_;
};

}  // namespace

HRESULT Nv12Converter::Init(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
  if (!dev) return E_INVALIDARG;
  dev_ = dev;
  ctx_ = ctx;
  if (!ctx_) dev_->GetImmediateContext(&ctx_);

  ComPtr<ID3DBlob> blob;
  HRESULT hr = Compile("VS_MAIN", "vs_5_0", blob);
  if (FAILED(hr)) return hr;
  hr = dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vs_);
  if (FAILED(hr)) return hr;
  hr = Compile("PS_LUMA", "ps_5_0", blob);
  if (FAILED(hr)) return hr;
  hr = dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                              &psLuma_);
  if (FAILED(hr)) return hr;
  hr = Compile("PS_CHROMA", "ps_5_0", blob);
  if (FAILED(hr)) return hr;
  hr = dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                              &psChroma_);
  if (FAILED(hr)) return hr;
  hr = Compile("PS_DEPTH", "ps_5_0", blob);
  if (FAILED(hr)) return hr;
  hr = dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                              &psDepth_);
  if (FAILED(hr)) return hr;

  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  hr = dev->CreateSamplerState(&sd, &sampler_);
  if (FAILED(hr)) return hr;

  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  hr = dev->CreateRasterizerState(&rd, &raster_);
  if (FAILED(hr)) return hr;

  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = sizeof(ConvertCB);
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  hr = dev->CreateBuffer(&bd, nullptr, &cb_);
  if (FAILED(hr)) return hr;
  bd.ByteWidth = sizeof(DepthCB);
  hr = dev->CreateBuffer(&bd, nullptr, &depthCb_);
  if (FAILED(hr)) return hr;
  D3D11_TEXTURE2D_DESC bt{};
  bt.Width = bt.Height = 1;
  bt.MipLevels = bt.ArraySize = 1;
  bt.Format = DXGI_FORMAT_R8_UNORM;
  bt.SampleDesc.Count = 1;
  bt.Usage = D3D11_USAGE_DEFAULT;
  bt.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  hr = dev->CreateTexture2D(&bt, nullptr, &blackTex_);
  if (FAILED(hr)) return hr;
  return dev->CreateShaderResourceView(blackTex_.Get(), nullptr, &blackSrv_);
}

HRESULT Nv12Converter::EnsureDepthTex(UINT mapW, UINT mapH) {
  if (depthTex_ && depthMapW_ == mapW && depthMapH_ == mapH) return S_OK;
  depthTex_.Reset();
  depthSrv_.Reset();
  D3D11_TEXTURE2D_DESC td{};
  td.Width = mapW;
  td.Height = mapH;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  HRESULT hr = dev_->CreateTexture2D(&td, nullptr, &depthTex_);
  if (FAILED(hr)) return hr;
  hr = dev_->CreateShaderResourceView(depthTex_.Get(), nullptr, &depthSrv_);
  if (FAILED(hr)) {
    depthTex_.Reset();
    return hr;
  }
  depthMapW_ = mapW;
  depthMapH_ = mapH;
  return S_OK;
}

HRESULT Nv12Converter::EnsureTargets(UINT w, UINT h) {
  if (w == outW_ && h == outH_ && targets_[0].tex) return S_OK;
  for (auto& t : targets_) t = {};
  outW_ = outH_ = 0;
  for (auto& t : targets_) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = dev_->CreateTexture2D(&td, nullptr, &t.tex);
    if (FAILED(hr)) return hr;
    D3D11_RENDER_TARGET_VIEW_DESC rv{};
    rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rv.Format = DXGI_FORMAT_R8_UNORM;
    hr = dev_->CreateRenderTargetView(t.tex.Get(), &rv, &t.luma);
    if (FAILED(hr)) return hr;
    rv.Format = DXGI_FORMAT_R8G8_UNORM;
    hr = dev_->CreateRenderTargetView(t.tex.Get(), &rv, &t.chroma);
    if (FAILED(hr)) return hr;
  }
  outW_ = w;
  outH_ = h;
  next_ = 0;
  return S_OK;
}

namespace {
HRESULT ValidateOverlay(const Nv12DepthOverlay& ov, UINT dstW, UINT dstH) {
  if (!ov.slices || !ov.mapWidth || !ov.mapHeight || !ov.sliceRows) return E_INVALIDARG;
  if (ov.colorHeight + 16 > ov.stripY) return E_INVALIDARG;
  if (ov.stripY % 8) return E_INVALIDARG;  // strip/chroma row alignment
  if (ov.mapHeight % ov.slices) return E_INVALIDARG;
  if (ov.mapWidth * ov.slices > dstW) return E_INVALIDARG;
  if (ov.stripY + ov.sliceRows > dstH) return E_INVALIDARG;
  return S_OK;
}
}  // namespace

HRESULT Nv12Converter::ConvertToTexture(ID3D11ShaderResourceView* srcSrv, UINT srcW,
                                        UINT srcH, UINT dstW, UINT dstH,
                                        ChromaFilter filter, ID3D11Texture2D** out) {
  return ConvertToTexture(srcSrv, srcW, srcH, dstW, dstH, filter, out, nullptr);
}

HRESULT Nv12Converter::ConvertToTexture(ID3D11ShaderResourceView* srcSrv, UINT srcW,
                                        UINT srcH, UINT dstW, UINT dstH,
                                        ChromaFilter filter, ID3D11Texture2D** out,
                                        const Nv12DepthOverlay* ov) {
  if (!vs_) return E_NOT_VALID_STATE;
  if (!srcSrv || !out || !srcW || !srcH || !dstW || !dstH || (dstW & 1) || (dstH & 1))
    return E_INVALIDARG;
  if (ov) {
    const HRESULT vr = ValidateOverlay(*ov, dstW, dstH);
    if (FAILED(vr)) return vr;
  }
  // Colour maps onto rows [0, colorH); with a depth overlay the target is
  // taller (guard band + strip + padding) and rows beyond colorH clamp to the
  // last source row.
  const UINT colorH = ov ? ov->colorHeight : dstH;
  // Without scaling the shader loads texels directly: the colour area must fit.
  const bool scaled = dstW != srcW || colorH != srcH;
  if (!scaled && ((srcW & 1) || (srcH & 1))) return E_INVALIDARG;

  SrwExclusive guard(lock_);
  HRESULT hr = EnsureTargets(dstW, dstH);
  if (FAILED(hr)) return hr;
  if (ov && ov->map) {
    hr = EnsureDepthTex(ov->mapWidth, ov->mapHeight);
    if (FAILED(hr)) return hr;
  }
  Target& t = targets_[next_];
  next_ = (next_ + 1) % kPoolSize;

  ContextLock ctxLock(ctx_.Get());
  ID3D11DeviceContext* ctx = ctx_.Get();
  ID3D11ShaderResourceView* nullSrv2[2] = {nullptr, nullptr};
  const ConvertCB cbb{float(dstW), float(colorH), UINT(filter), scaled ? 1u : 0u,
                      srcH - 1, 0};
  ctx->UpdateSubresource(cb_.Get(), 0, nullptr, &cbb, 0, 0);

  ctx->IASetInputLayout(nullptr);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(vs_.Get(), nullptr, 0);
  ctx->RSSetState(raster_.Get());
  ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
  ctx->OMSetDepthStencilState(nullptr, 0);
  // Slots t0 (colour) and t1 (depth) are always bound: the passes that do not
  // read one get the 1×1 stand-in rather than a hole.
  ID3D11ShaderResourceView* colorSrvs[2] = {srcSrv, blackSrv_.Get()};
  ctx->PSSetShaderResources(0, 2, colorSrvs);
  ctx->PSSetSamplers(0, 1, sampler_.GetAddressOf());
  ctx->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());

  if (ov) {
    // The strip and the padding below it are not drawn over by the colour
    // pass, so start from black luma and neutral chroma; the colour pass then
    // covers [0, stripY), the depth pass overwrites the strip's luma.
    const float black[4] = {0.f, 0.f, 0.f, 0.f};
    const float neutral[4] = {128.f / 255.f, 128.f / 255.f, 0.f, 0.f};
    ctx->ClearRenderTargetView(t.luma.Get(), black);
    ctx->ClearRenderTargetView(t.chroma.Get(), neutral);
    if (ov->map)
      ctx->UpdateSubresource(depthTex_.Get(), 0, nullptr, ov->map, ov->mapWidth, 0);
  }

  const UINT lumaRows = ov ? ov->stripY : dstH;  // colour + guard band
  // Chroma covers everything above the strip's first FULL chroma row; the
  // half row where an odd sliceRows straddles stripY keeps the clear's
  // neutral value, so the depth region stays self-contained.
  const UINT chromaRows = ov ? ov->stripY / 2 + (ov->stripY % 2) : dstH / 2;
  D3D11_VIEWPORT vp{0, 0, float(dstW), float(lumaRows), 0, 1};
  ctx->RSSetViewports(1, &vp);
  ctx->OMSetRenderTargets(1, t.luma.GetAddressOf(), nullptr);
  ctx->PSSetShader(psLuma_.Get(), nullptr, 0);
  ctx->Draw(3, 0);

  vp.Width = float(dstW / 2);
  vp.Height = float(chromaRows);
  ctx->RSSetViewports(1, &vp);
  ctx->OMSetRenderTargets(1, t.chroma.GetAddressOf(), nullptr);
  ctx->PSSetShader(psChroma_.Get(), nullptr, 0);
  ctx->Draw(3, 0);

  if (ov) {
    // Depth strip: sliced map, luma only (its chroma stays neutral 128, so it
    // costs almost nothing to code).
    const DepthCB dcb{ov->mapWidth, ov->sliceRows, ov->map ? 0u : 1u, ov->stripY};
    ctx->UpdateSubresource(depthCb_.Get(), 0, nullptr, &dcb, 0, 0);
    ctx->PSSetConstantBuffers(1, 1, depthCb_.GetAddressOf());
    ID3D11ShaderResourceView* depthSrvs[2] = {blackSrv_.Get(),
                                              ov->map ? depthSrv_.Get() : blackSrv_.Get()};
    ctx->PSSetShaderResources(0, 2, depthSrvs);
    vp.TopLeftX = 0;
    vp.TopLeftY = float(ov->stripY);
    vp.Width = float(ov->mapWidth * ov->slices);
    vp.Height = float(ov->sliceRows);
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, t.luma.GetAddressOf(), nullptr);
    ctx->PSSetShader(psDepth_.Get(), nullptr, 0);
    ctx->Draw(3, 0);
    ID3D11Buffer* nullCb[1] = {nullptr};
    ctx->PSSetConstantBuffers(1, 1, nullCb);
  }

  // Unbind so the ring slot can be a copy destination again and the encoder
  // can take the target; submit now rather than when a buffer happens to fill.
  ID3D11RenderTargetView* nullRtv[1] = {nullptr};
  ctx->OMSetRenderTargets(1, nullRtv, nullptr);
  ctx->PSSetShaderResources(0, 2, nullSrv2);
  ctx->Flush();

  *out = t.tex.Get();
  return S_OK;
}

HRESULT Nv12Converter::Readback(ID3D11Texture2D* nv12, std::vector<uint8_t>& nv12Out) {
  if (!nv12 || !dev_) return E_INVALIDARG;
  D3D11_TEXTURE2D_DESC td{};
  nv12->GetDesc(&td);
  if (td.Format != DXGI_FORMAT_NV12) return E_INVALIDARG;

  SrwExclusive guard(lock_);
  if (!staging_ || stagingW_ != td.Width || stagingH_ != td.Height) {
    staging_.Reset();
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.MiscFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    const HRESULT hr = dev_->CreateTexture2D(&sd, nullptr, &staging_);
    if (FAILED(hr)) return hr;
    stagingW_ = td.Width;
    stagingH_ = td.Height;
  }
  ctx_->CopyResource(staging_.Get(), nv12);
  D3D11_MAPPED_SUBRESOURCE mp{};
  const HRESULT hr = ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mp);
  if (FAILED(hr)) return hr;

  // A mapped NV12 texture is the Y plane followed by the interleaved UV plane,
  // both with the same row pitch.
  const UINT w = td.Width, h = td.Height;
  nv12Out.resize(Nv12Size(w, h));
  const uint8_t* src = static_cast<const uint8_t*>(mp.pData);
  uint8_t* dst = nv12Out.data();
  if (mp.RowPitch == w) {
    memcpy(dst, src, nv12Out.size());
  } else {
    for (UINT y = 0; y < h + h / 2; ++y)
      memcpy(dst + size_t(y) * w, src + size_t(y) * mp.RowPitch, w);
  }
  ctx_->Unmap(staging_.Get(), 0);
  return S_OK;
}

HRESULT Nv12Converter::Convert(ID3D11ShaderResourceView* srcSrv, UINT width, UINT height,
                               ChromaFilter filter, std::vector<uint8_t>& nv12Out) {
  ID3D11Texture2D* tex = nullptr;
  const HRESULT hr = ConvertToTexture(srcSrv, width, height, width, height, filter, &tex);
  if (FAILED(hr)) return hr;
  return Readback(tex, nv12Out);
}

}  // namespace wvd

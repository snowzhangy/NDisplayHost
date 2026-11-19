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
  float2 dstSize;   // luma size of the NV12 target, pixels
  uint filter;
  uint scaled;      // 0 = source and target are the same size
};

Texture2D<float4> srcTex : register(t0);
SamplerState linearClamp : register(s0);

static const float KR = 0.2126;
static const float KB = 0.0722;

float4 VS_MAIN(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

// Source colour for one target luma pixel. BGRA SRVs expose logical RGBA.
float3 Fetch(int2 lumaPix) {
  if (scaled != 0u)
    return srcTex.SampleLevel(linearClamp, (float2(lumaPix) + 0.5) / dstSize, 0).rgb;
  return srcTex.Load(int3(lumaPix, 0)).rgb;
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
)HLSL";

namespace {

struct alignas(16) ConvertCB {
  float dstW, dstH;
  UINT filter, scaled;
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
  return dev->CreateBuffer(&bd, nullptr, &cb_);
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

HRESULT Nv12Converter::ConvertToTexture(ID3D11ShaderResourceView* srcSrv, UINT srcW,
                                        UINT srcH, UINT dstW, UINT dstH,
                                        ChromaFilter filter, ID3D11Texture2D** out) {
  if (!vs_) return E_NOT_VALID_STATE;
  if (!srcSrv || !out || !srcW || !srcH || !dstW || !dstH || (dstW & 1) || (dstH & 1))
    return E_INVALIDARG;
  // Without scaling the shader loads texels directly: the target must fit.
  const bool scaled = dstW != srcW || dstH != srcH;
  if (!scaled && ((srcW & 1) || (srcH & 1))) return E_INVALIDARG;

  SrwExclusive guard(lock_);
  HRESULT hr = EnsureTargets(dstW, dstH);
  if (FAILED(hr)) return hr;
  Target& t = targets_[next_];
  next_ = (next_ + 1) % kPoolSize;

  ContextLock ctxLock(ctx_.Get());
  ID3D11DeviceContext* ctx = ctx_.Get();
  const ConvertCB cbb{float(dstW), float(dstH), UINT(filter), scaled ? 1u : 0u};
  ctx->UpdateSubresource(cb_.Get(), 0, nullptr, &cbb, 0, 0);

  ctx->IASetInputLayout(nullptr);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(vs_.Get(), nullptr, 0);
  ctx->RSSetState(raster_.Get());
  ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
  ctx->OMSetDepthStencilState(nullptr, 0);
  ctx->PSSetShaderResources(0, 1, &srcSrv);
  ctx->PSSetSamplers(0, 1, sampler_.GetAddressOf());
  ctx->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());

  D3D11_VIEWPORT vp{0, 0, float(dstW), float(dstH), 0, 1};
  ctx->RSSetViewports(1, &vp);
  ctx->OMSetRenderTargets(1, t.luma.GetAddressOf(), nullptr);
  ctx->PSSetShader(psLuma_.Get(), nullptr, 0);
  ctx->Draw(3, 0);

  vp.Width = float(dstW / 2);
  vp.Height = float(dstH / 2);
  ctx->RSSetViewports(1, &vp);
  ctx->OMSetRenderTargets(1, t.chroma.GetAddressOf(), nullptr);
  ctx->PSSetShader(psChroma_.Get(), nullptr, 0);
  ctx->Draw(3, 0);

  // Unbind so the ring slot can be a copy destination again and the encoder
  // can take the target; submit now rather than when a buffer happens to fill.
  ID3D11RenderTargetView* nullRtv[1] = {nullptr};
  ID3D11ShaderResourceView* nullSrv[1] = {nullptr};
  ctx->OMSetRenderTargets(1, nullRtv, nullptr);
  ctx->PSSetShaderResources(0, 1, nullSrv);
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

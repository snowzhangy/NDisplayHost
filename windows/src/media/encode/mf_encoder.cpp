#include "mf_encoder.h"
#include <cmath>

// clang-format off
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <codecapi.h>
#include <d3d11.h>
// clang-format on
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <utility>

#include "wvd/annex_b.h"
#include "wvd/logging.h"
#include "wvd/time.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace wvd {

uint32_t MfFrameBufferBytes(uint32_t kbps, uint32_t fpsNum, uint32_t fpsDen) {
  if (!kbps || !fpsNum || !fpsDen) return 0;
  const long double bytes = static_cast<long double>(kbps) * 1000 * fpsDen / (8.0L * fpsNum);
  return uint32_t((std::max)(1.0L, (std::min)(static_cast<long double>(UINT32_MAX), std::ceil(bytes))));
}

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Annex B helpers
// ---------------------------------------------------------------------------

namespace {

bool IsKeySlice(Codec c, int t) {
  return c == Codec::H264 ? t == 5 : (t >= 16 && t <= 21);  // HEVC IRAP: BLA/IDR/CRA
}

}  // namespace

std::vector<uint8_t> ExtractParameterSets(Codec codec, const uint8_t* au, size_t len) {
  std::vector<uint8_t> out;
  if (!au || !len) return out;
  ForEachAnnexBNal(au, len, [&](const uint8_t* nal, size_t size) {
    if (IsAnnexBParameterSet(codec, AnnexBNalType(codec, nal[0]))) {
      static const uint8_t kSc[4] = {0, 0, 0, 1};
      out.insert(out.end(), kSc, kSc + 4);
      out.insert(out.end(), nal, nal + size);
    }
  });
  return out;
}

bool ContainsKeyframe(Codec codec, const uint8_t* au, size_t len) {
  bool key = false;
  if (!au || !len) return false;
  ForEachAnnexBNal(au, len, [&](const uint8_t* nal, size_t) {
    if (IsKeySlice(codec, AnnexBNalType(codec, nal[0]))) key = true;
  });
  return key;
}

// ---------------------------------------------------------------------------
// Media Foundation plumbing
// ---------------------------------------------------------------------------

namespace {

bool EnsureMf() {
  // Never shut down: encoders can be re-created for every session and
  // MFShutdown with live objects is undefined.
  static const bool ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET));
  return ok;
}

void EnsureComOnThisThread() {
  // MF objects are free-threaded; every thread that calls into them needs COM.
  thread_local const bool done = [] {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
  }();
  (void)done;
}

const char* CodecName(Codec c) {
  switch (c) {
    case Codec::H264: return "H.264";
    case Codec::AV1: return "AV1";
    default: return "HEVC";
  }
}

std::string FriendlyName(IMFActivate* a) {
  WCHAR* s = nullptr;
  UINT32 len = 0;
  std::string r = "?";
  if (a && SUCCEEDED(a->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &s, &len)) && s) {
    r = WideToUtf8(s);
    CoTaskMemFree(s);
  }
  return r;
}

Vendor VendorOf(IMFActivate* a, bool hardware) {
  if (!hardware) return Vendor::Microsoft;
  WCHAR* s = nullptr;
  UINT32 len = 0;
  Vendor v = Vendor::Microsoft;
  if (a && SUCCEEDED(a->GetAllocatedString(MFT_ENUM_HARDWARE_VENDOR_ID_Attribute, &s, &len)) &&
      s) {
    std::wstring id = s;
    CoTaskMemFree(s);
    for (auto& ch : id) ch = wchar_t(towupper(ch));
    if (id.find(L"1002") != std::wstring::npos || id.find(L"1022") != std::wstring::npos)
      v = Vendor::AMD;
    else if (id.find(L"10DE") != std::wstring::npos)
      v = Vendor::NVIDIA;
    else if (id.find(L"8086") != std::wstring::npos)
      v = Vendor::Intel;
    else if (id.find(L"QCOM") != std::wstring::npos || id.find(L"5143") != std::wstring::npos)
      v = Vendor::Qualcomm;
  }
  return v;
}

std::vector<ComPtr<IMFActivate>> EnumEncoders(const GUID& outSubtype, bool hardware,
                                              const LUID& luid) {
  std::vector<ComPtr<IMFActivate>> result;
  if (!EnsureMf()) return result;
  EnsureComOnThisThread();
  MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12};
  MFT_REGISTER_TYPE_INFO out{MFMediaType_Video, outSubtype};
  const UINT32 flags =
      MFT_ENUM_FLAG_SORTANDFILTER |
      (hardware ? MFT_ENUM_FLAG_HARDWARE : (MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT));
  IMFActivate** acts = nullptr;
  UINT32 n = 0;
  HRESULT hr = E_FAIL;
  if (hardware && (luid.LowPart || luid.HighPart)) {
    // Only the encoder on the GPU that owns the captured output: a second GPU's
    // MFT would force a cross-adapter copy.
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(MFCreateAttributes(&attrs, 1))) {
      const UINT64 v = (UINT64(uint32_t(luid.HighPart)) << 32) | luid.LowPart;
      attrs->SetUINT64(MFT_ENUM_ADAPTER_LUID, v);
      hr = MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, flags, &in, &out, attrs.Get(), &acts, &n);
    }
  }
  if (FAILED(hr) || n == 0) {
    if (acts) {
      for (UINT32 i = 0; i < n; ++i) acts[i]->Release();
      CoTaskMemFree(acts);
      acts = nullptr;
    }
    n = 0;
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &in, &out, &acts, &n);
  }
  if (SUCCEEDED(hr) && acts) {
    for (UINT32 i = 0; i < n; ++i) {
      result.emplace_back(acts[i]);  // ComPtr adds its own reference
      acts[i]->Release();
    }
  }
  if (acts) CoTaskMemFree(acts);
  return result;
}

// Async (hardware) MFTs report "need input" / "have output" through their
// event queue. The pump keeps one BeginGetEvent outstanding and counts the
// events; the encode thread waits on `signal`.
struct EventState {
  std::mutex m;
  int needInput = 0;
  int haveOutput = 0;
  HRESULT error = S_OK;
  HANDLE signal = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  std::atomic<bool> stopping{false};
  ~EventState() {
    if (signal) CloseHandle(signal);
  }
};

class EventPump final : public IMFAsyncCallback {
 public:
  EventPump(ComPtr<IMFMediaEventGenerator> gen, std::shared_ptr<EventState> st)
      : gen_(std::move(gen)), st_(std::move(st)) {}

  HRESULT Start() { return gen_->BeginGetEvent(this, nullptr); }

  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAsyncCallback)) {
      *ppv = static_cast<IMFAsyncCallback*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG r = --ref_;
    if (!r) delete this;
    return r;
  }
  STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }

  STDMETHODIMP Invoke(IMFAsyncResult* res) override {
    ComPtr<IMFMediaEventGenerator> gen = gen_;
    if (!gen) return S_OK;
    ComPtr<IMFMediaEvent> ev;
    HRESULT hr = gen->EndGetEvent(res, &ev);
    if (FAILED(hr)) {
      {
        std::lock_guard<std::mutex> lk(st_->m);
        if (hr != MF_E_SHUTDOWN && !st_->stopping) st_->error = hr;
      }
      SetEvent(st_->signal);
      gen_.Reset();
      return S_OK;
    }
    MediaEventType type = MEUnknown;
    ev->GetType(&type);
    HRESULT status = S_OK;
    ev->GetStatus(&status);
    {
      std::lock_guard<std::mutex> lk(st_->m);
      switch (type) {
        case METransformNeedInput: ++st_->needInput; break;
        case METransformHaveOutput: ++st_->haveOutput; break;
        case MEError: st_->error = FAILED(status) ? status : E_FAIL; break;
        default: break;
      }
    }
    SetEvent(st_->signal);
    if (st_->stopping || FAILED(gen->BeginGetEvent(this, nullptr))) gen_.Reset();
    return S_OK;
  }

 private:
  std::atomic<ULONG> ref_{1};
  ComPtr<IMFMediaEventGenerator> gen_;
  std::shared_ptr<EventState> st_;
};

DWORD MsLeft(ULONGLONG deadline) {
  const ULONGLONG now = GetTickCount64();
  return now >= deadline ? 0 : DWORD(deadline - now);
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

class MfEncoder final : public IVideoEncoder {
 public:
  MfEncoder(const EncoderInitDesc& d, bool hardware) : desc_(d), hardware_(hardware) {
    caps_.maxFrameWidth = 8192;
    caps_.maxFrameHeight = 4320;
  }
  ~MfEncoder() override { Shutdown(); }

  const char* name() const override { return hardware_ ? "mf-hw" : "mf-sw-h264"; }
  Vendor vendor() const override { return vendor_; }
  uint8_t tier() const override { return hardware_ ? 3 : 4; }
  const EncoderCaps& caps() const override { return caps_; }
  Codec codec() const override { return codec_; }
  std::vector<uint8_t> codecPrivate() const override { return csd_; }
  HRESULT ForceKeyframe() override {
    forceKey_ = true;
    return S_OK;
  }
  bool zeroCopy() const override { return gpu_; }

  HRESULT SetBitrate(uint32_t kbps) override {
    if (!codecApi_ || !kbps) return E_NOTIMPL;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = kbps * 1000;
    const HRESULT hr = codecApi_->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);
    if (FAILED(hr)) return hr;
    desc_.bitrateKbps = kbps;
    // Keep the ~1 frame VBV in step with the new rate (best effort).
    SetCodecU32(CODECAPI_AVEncCommonBufferSize, MfFrameBufferBytes(kbps, desc_.fpsNum, desc_.fpsDen));
    return S_OK;
  }

  HRESULT Init() {
    if (!EnsureMf()) return E_FAIL;
    EnsureComOnThisThread();
    if (!desc_.width || !desc_.height || (desc_.width & 1) || (desc_.height & 1) ||
        !desc_.fpsNum || !desc_.fpsDen)
      return E_INVALIDARG;

    std::vector<std::pair<Codec, GUID>> order;
    if (!hardware_)
      order = {{Codec::H264, MFVideoFormat_H264}};
    else if (desc_.codec == Codec::H264)
      order = {{Codec::H264, MFVideoFormat_H264}, {Codec::HEVC, MFVideoFormat_HEVC}};
    else
      order = {{Codec::HEVC, MFVideoFormat_HEVC}, {Codec::H264, MFVideoFormat_H264}};

    // Texture input first (the picture never leaves the GPU), then the same
    // MFT fed from CPU buffers.
    const bool tryGpu = hardware_ && gpuAllowed_ && desc_.device;
    for (auto& [c, sub] : order) {
      for (auto& act : EnumEncoders(sub, hardware_, desc_.adapterLuid)) {
        for (const bool gpu : {true, false}) {
          if (gpu && !tryGpu) continue;
          const HRESULT hr = TryActivate(act.Get(), c, sub, gpu);
          if (SUCCEEDED(hr)) return S_OK;
          if (gpu)
            WVD_LOGI("MF encoder '%s' (%s) declined texture input (hr=%s) — using CPU buffers",
                     FriendlyName(act.Get()).c_str(), CodecName(c), HresultStr(hr).c_str());
          else
            WVD_LOGW("MF encoder '%s' (%s) rejected %ux%u@%u: hr=%s — trying the next one",
                     FriendlyName(act.Get()).c_str(), CodecName(c), desc_.width, desc_.height,
                     Fps(), HresultStr(hr).c_str());
          Shutdown();
        }
      }
    }
    return MF_E_TOPO_CODEC_NOT_FOUND;
  }

  HRESULT Encode(const SourcePicture& src, EncodedAu& au) override {
    EnsureComOnThisThread();
    if (!mft_) return E_NOT_VALID_STATE;
    au.bytes.clear();
    au.key = false;
    au.qpcEncodeIn = QpcNow();

    HRESULT hr = EncodeOnce(src, au);
    if (FAILED(hr) && gpu_ && !producedOutput_) {
      // The MFT accepted a D3D manager but not our textures. Rebuild it for
      // CPU input once instead of dropping to a lower tier.
      WVD_LOGW("MF encoder '%s': texture input failed (hr=%s) — retrying with CPU buffers",
               friendly_.c_str(), HresultStr(hr).c_str());
      Shutdown();
      gpuAllowed_ = false;
      forceKey_ = true;
      au.bytes.clear();
      hr = Init();
      if (SUCCEEDED(hr)) hr = EncodeOnce(src, au);
    }
    if (FAILED(hr)) return hr;
    if (!au.bytes.empty()) {
      producedOutput_ = true;
      au.key = au.key || ContainsKeyframe(codec_, au.bytes.data(), au.bytes.size());
      // An async MFT can return an EARLIER picture than the one just
      // submitted; AppendSample resolved that picture's identity from the
      // sample time. Only a picture we could not match falls back to the
      // current input (never stamp a lagged output with fresh identity).
      if (!au.qpcCapture) {
        au.qpcCapture = src.qpcCapture;
        au.contentVer = src.contentVer;
        au.refinement = src.qualityBoost;
        au.depthValid = src.depthValid;
      }
    }
    au.qpcEncodeOut = QpcNow();
    return S_OK;
  }

 private:
  UINT32 Fps() const { return (std::max)(1u, desc_.fpsNum / desc_.fpsDen); }

  HRESULT MakeInputBuffer(const SourcePicture& src, ComPtr<IMFMediaBuffer>& buf) {
    if (gpu_) {
      if (!src.texture) return E_INVALIDARG;
      D3D11_TEXTURE2D_DESC td{};
      src.texture->GetDesc(&td);
      if (td.Width != desc_.width || td.Height != desc_.height) return E_INVALIDARG;
      HRESULT hr =
          MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), src.texture, 0, FALSE, &buf);
      if (FAILED(hr)) return hr;
      ComPtr<IMF2DBuffer> b2;
      DWORD len = 0;
      if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->GetContiguousLength(&len)))
        buf->SetCurrentLength(len);
      return S_OK;
    }
    const size_t need = size_t(desc_.width) * desc_.height * 3 / 2;
    const uint8_t* nv12 = nullptr;
    size_t nv12Size = 0;
    HRESULT hr = src.Cpu(&nv12, &nv12Size);
    if (FAILED(hr)) return hr;
    if (nv12Size < need) return E_INVALIDARG;
    hr = MFCreateMemoryBuffer(DWORD(need), &buf);
    if (FAILED(hr)) return hr;
    BYTE* p = nullptr;
    hr = buf->Lock(&p, nullptr, nullptr);
    if (FAILED(hr)) return hr;
    memcpy(p, nv12, need);
    buf->Unlock();
    buf->SetCurrentLength(DWORD(need));
    return S_OK;
  }

  HRESULT EncodeOnce(const SourcePicture& src, EncodedAu& au) {
    if (!mft_) return E_NOT_VALID_STATE;
    ComPtr<IMFMediaBuffer> buf;
    HRESULT hr = MakeInputBuffer(src, buf);
    if (FAILED(hr)) return hr;

    ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) return hr;
    sample->AddBuffer(buf.Get());
    const LONGLONG dur = 10'000'000LL * desc_.fpsDen / desc_.fpsNum;
    const LONGLONG time = frameIndex_ * dur;
    sample->SetSampleTime(time);
    sample->SetSampleDuration(dur);
    ++frameIndex_;

    if (forceKey_.exchange(false) && codecApi_) {
      VARIANT v;
      VariantInit(&v);
      v.vt = VT_UI4;
      v.ulVal = 1;
      codecApi_->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
    }

    return async_ ? EncodeAsync(sample.Get(), src, time, au)
                  : EncodeSync(sample.Get(), src, time, au);
  }

  void SetCodecU32(const GUID& api, UINT32 value) {
    if (!codecApi_) return;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    codecApi_->SetValue(&api, &v);  // best effort: not every MFT supports every knob
  }
  void SetCodecBool(const GUID& api, bool value) {
    if (!codecApi_) return;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    codecApi_->SetValue(&api, &v);
  }

  // §6.7 SDR reference: BT.709 primaries/matrix, full range (the NV12 stage
  // emits full-range samples).
  void SetColor(IMFMediaType* t) {
    t->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    t->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_0_255);
    t->SetUINT32(MF_MT_VIDEO_CHROMA_SITING, MFVideoChromaSubsampling_MPEG2);
  }

  void ReadOutputTypeState() {
    ComPtr<IMFMediaType> cur;
    if (SUCCEEDED(mft_->GetOutputCurrentType(outId_, &cur))) {
      UINT32 sz = 0;
      if (SUCCEEDED(cur->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &sz)) && sz) {
        std::vector<uint8_t> blob(sz);
        if (SUCCEEDED(cur->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, blob.data(), sz, nullptr))) {
          auto ps = ExtractParameterSets(codec_, blob.data(), blob.size());
          csd_ = ps.empty() ? blob : ps;
        }
      }
    }
    MFT_OUTPUT_STREAM_INFO osi{};
    if (SUCCEEDED(mft_->GetOutputStreamInfo(outId_, &osi))) {
      mftProvidesSamples_ =
          (osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                          MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
      outBufSize_ = osi.cbSize;
    }
    if (outBufSize_ < 64 * 1024)
      outBufSize_ = (std::max)(DWORD(desc_.width * desc_.height * 3 / 2), DWORD(64 * 1024));
  }

  HRESULT TryActivate(IMFActivate* act, Codec c, const GUID& sub, bool gpu) {
    activate_ = act;
    HRESULT hr = act->ActivateObject(IID_PPV_ARGS(&mft_));
    if (FAILED(hr)) return hr;
    friendly_ = FriendlyName(act);
    vendor_ = VendorOf(act, hardware_);
    codec_ = c;

    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(mft_->GetAttributes(&attrs)) && attrs) {
      UINT32 isAsync = 0;
      if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync)) && isAsync) {
        hr = attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        if (FAILED(hr)) return hr;
        async_ = true;
      }
      attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    }

    if (gpu) {
      // Hand the MFT the device the pictures live on. Must precede the media
      // types; an MFT that is not D3D11-aware fails here and gets CPU buffers.
      UINT32 aware = 0;
      if (!attrs || FAILED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &aware)) || !aware)
        return MF_E_UNSUPPORTED_D3D_TYPE;
      hr = MFCreateDXGIDeviceManager(&resetToken_, &devMgr_);
      if (FAILED(hr)) return hr;
      hr = devMgr_->ResetDevice(desc_.device, resetToken_);
      if (FAILED(hr)) return hr;
      hr = mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                reinterpret_cast<ULONG_PTR>(devMgr_.Get()));
      if (FAILED(hr)) return hr;
    }
    gpu_ = gpu;

    DWORD inIds[1] = {0}, outIds[1] = {0};
    if (SUCCEEDED(mft_->GetStreamIDs(1, inIds, 1, outIds))) {
      inId_ = inIds[0];
      outId_ = outIds[0];
    } else {
      inId_ = outId_ = 0;  // E_NOTIMPL: fixed streams numbered from zero
    }

    mft_.As(&codecApi_);
    const UINT32 fps = Fps();
    // Low latency first: some MFTs reset other knobs when it changes.
    SetCodecBool(CODECAPI_AVLowLatencyMode, true);
    SetCodecU32(CODECAPI_AVEncCommonQualityVsSpeed, desc_.preferQuality ? 70 : 0);
    SetCodecU32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
    SetCodecU32(CODECAPI_AVEncCommonMeanBitRate, desc_.bitrateKbps * 1000);
    SetCodecU32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    // Keyframes on demand only unless a period was asked for: the session
    // forces one for the first picture and for every recovery request.
    SetCodecU32(CODECAPI_AVEncMPVGOPSize,
                desc_.gopSeconds ? fps * desc_.gopSeconds : kOnDemandGop);
    // H.264/HEVC use bytes here. Request ~1 frame of HRD buffering to limit
    // bursts (§6.6); hardware MFTs can decline this optional property.
    SetCodecU32(CODECAPI_AVEncCommonBufferSize,
                MfFrameBufferBytes(desc_.bitrateKbps, desc_.fpsNum, desc_.fpsDen));

    ComPtr<IMFMediaType> out;
    hr = MFCreateMediaType(&out);
    if (FAILED(hr)) return hr;
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, sub);
    out->SetUINT32(MF_MT_AVG_BITRATE, desc_.bitrateKbps * 1000);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, desc_.width, desc_.height);
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, desc_.fpsNum, desc_.fpsDen);
    MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, c == Codec::HEVC ? UINT32(eAVEncH265VProfile_Main_420_8)
                                                         : UINT32(eAVEncH264VProfile_High));
    SetColor(out.Get());
    hr = mft_->SetOutputType(outId_, out.Get(), 0);
    if (FAILED(hr) && c == Codec::H264) {
      out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
      hr = mft_->SetOutputType(outId_, out.Get(), 0);
    }
    if (FAILED(hr)) return hr;

    // Input: start from the MFT's own NV12 type so vendor-specific attributes
    // survive, then pin size/rate/stride/colour.
    ComPtr<IMFMediaType> in;
    for (DWORD i = 0;; ++i) {
      ComPtr<IMFMediaType> t;
      if (FAILED(mft_->GetInputAvailableType(inId_, i, &t))) break;
      GUID st{};
      if (SUCCEEDED(t->GetGUID(MF_MT_SUBTYPE, &st)) && st == MFVideoFormat_NV12) {
        in = t;
        break;
      }
    }
    if (!in) {
      hr = MFCreateMediaType(&in);
      if (FAILED(hr)) return hr;
      in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
      in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    }
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, desc_.width, desc_.height);
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, desc_.fpsNum, desc_.fpsDen);
    MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    in->SetUINT32(MF_MT_DEFAULT_STRIDE, desc_.width);  // packed NV12, pitch = width
    SetColor(in.Get());
    hr = mft_->SetInputType(inId_, in.Get(), 0);
    if (FAILED(hr)) return hr;

    ReadOutputTypeState();

    if (async_) {
      ComPtr<IMFMediaEventGenerator> gen;
      hr = mft_.As(&gen);
      if (FAILED(hr)) return hr;
      events_ = std::make_shared<EventState>();
      pump_ = new EventPump(gen, events_);
      hr = pump_->Start();
      if (FAILED(hr)) return hr;
    }
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    hr = mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) return hr;

    caps_.h264 = c == Codec::H264;
    caps_.hevc8 = c == Codec::HEVC;
    WVD_LOGI("MF encoder ready: '%s' %s %s %ux%u@%u %ukbps csd=%zuB input=%s",
             friendly_.c_str(), async_ ? "async-hw" : (hardware_ ? "sync-hw" : "software"),
             CodecName(c), desc_.width, desc_.height, fps, desc_.bitrateKbps, csd_.size(),
             gpu_ ? "D3D11 texture (zero-copy)" : "CPU buffer");
    return S_OK;
  }

  void Shutdown() {
    if (events_) events_->stopping = true;
    if (mft_) {
      mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
      mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
      ComPtr<IMFShutdown> sd;
      if (SUCCEEDED(mft_.As(&sd))) sd->Shutdown();  // completes the pending event read
    }
    codecApi_.Reset();
    mft_.Reset();
    devMgr_.Reset();
    gpu_ = false;
    if (activate_) {
      activate_->ShutdownObject();
      activate_.Reset();
    }
    if (pump_) {
      pump_->Release();
      pump_ = nullptr;
    }
    events_.reset();
    async_ = false;
    csd_.clear();
    pending_.clear();
    pendingOut_.clear();
  }

  // Encode() runs single-threaded on the session's encode thread, like
  // frameIndex_, so these two queues need no lock of their own.
  struct Pending {
    int64_t qpcCapture = 0;
    uint64_t contentVer = 0;
    bool refinement = false;
    bool depthValid = false;
    LONGLONG time = 0;
  };
  void RegisterPending(const SourcePicture& src, LONGLONG time) {
    pending_.push_back(
        Pending{src.qpcCapture, src.contentVer, src.qualityBoost, src.depthValid, time});
    // An input that never produced output must not age the queue forever.
    while (pending_.size() > 64) pending_.pop_front();
  }

  // Resolve WHICH input picture this output belongs to: by sample time when
  // the MFT preserves it, else in submission order. A lagged output must
  // carry the identity of ITS input, not the one currently being submitted.
  void AppendSample(IMFSample* s, EncodedAu& au) {
    if (!s) return;
    LONGLONG t = 0;
    const bool haveTime = SUCCEEDED(s->GetSampleTime(&t));
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
      if ((haveTime && it->time == t) || (!haveTime && it == pending_.begin())) {
        au.contentVer = it->contentVer;
        au.qpcCapture = it->qpcCapture;
        au.refinement = it->refinement;
        au.depthValid = it->depthValid;
        pending_.erase(it);
        break;
      }
    }
    ComPtr<IMFMediaBuffer> b;
    if (FAILED(s->ConvertToContiguousBuffer(&b))) return;
    BYTE* p = nullptr;
    DWORD len = 0;
    if (SUCCEEDED(b->Lock(&p, nullptr, &len))) {
      au.bytes.insert(au.bytes.end(), p, p + len);
      b->Unlock();
    }
    UINT32 clean = 0;
    if (SUCCEEDED(s->GetUINT32(MFSampleExtension_CleanPoint, &clean)) && clean) au.key = true;
  }

  // S_OK: one output appended. S_FALSE: output format renegotiated, call again.
  // MF_E_TRANSFORM_NEED_MORE_INPUT: nothing pending.
  HRESULT PullOneOutput(EncodedAu& au) {
    MFT_OUTPUT_DATA_BUFFER ob{};
    ob.dwStreamID = outId_;
    ComPtr<IMFSample> own;
    if (!mftProvidesSamples_) {
      ComPtr<IMFMediaBuffer> b;
      HRESULT hr = MFCreateMemoryBuffer(outBufSize_, &b);
      if (FAILED(hr)) return hr;
      hr = MFCreateSample(&own);
      if (FAILED(hr)) return hr;
      own->AddBuffer(b.Get());
      ob.pSample = own.Get();
    }
    DWORD status = 0;
    HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
    if (ob.pEvents) {
      ob.pEvents->Release();
      ob.pEvents = nullptr;
    }
    IMFSample* produced = ob.pSample;
    const bool mftOwned = mftProvidesSamples_ && produced;
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      if (mftOwned) produced->Release();
      ComPtr<IMFMediaType> t;
      HRESULT h2 = mft_->GetOutputAvailableType(outId_, 0, &t);
      if (SUCCEEDED(h2)) h2 = mft_->SetOutputType(outId_, t.Get(), 0);
      if (FAILED(h2)) return h2;
      ReadOutputTypeState();
      return S_FALSE;
    }
    if (FAILED(hr)) {
      if (mftOwned) produced->Release();
      return hr;
    }
    AppendSample(produced, au);
    if (mftOwned) produced->Release();
    return S_OK;
  }

  // One complete access unit per OUTPUT PICTURE. Outputs pulled beyond the
  // one this call returns are parked in pendingOut_ and handed out by the
  // next calls — never concatenated into a single AU, whose packet header
  // could only describe one of the pictures inside it.
  HRESULT TakeOne(EncodedAu& au) {
    if (pendingOut_.empty()) return S_OK;
    au = std::move(pendingOut_.front());
    pendingOut_.pop_front();
    return S_OK;
  }

  HRESULT ParkOutput(EncodedAu&& one) {
    if (one.bytes.empty()) return S_OK;
    pendingOut_.push_back(std::move(one));
    return S_OK;
  }

  HRESULT EncodeSync(IMFSample* s, const SourcePicture& src, LONGLONG time, EncodedAu& au) {
    HRESULT hr = mft_->ProcessInput(inId_, s, 0);
    if (hr == MF_E_NOTACCEPTING) {
      DrainSync(nullptr);
      hr = mft_->ProcessInput(inId_, s, 0);
    }
    if (FAILED(hr)) return hr;
    RegisterPending(src, time);
    return DrainSync(&au);
  }

  HRESULT DrainSync(EncodedAu* au) {
    // A picture collected earlier leaves first: output order is decode order.
    if (au && !pendingOut_.empty()) TakeOne(*au);
    for (int i = 0; i < 8; ++i) {
      if (au && !au->bytes.empty()) break;  // this call's picture collected
      EncodedAu one;
      const HRESULT hr = PullOneOutput(one);
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
      if (FAILED(hr)) return hr;
      if (au && au->bytes.empty())
        *au = std::move(one);
      else
        ParkOutput(std::move(one));
    }
    return S_OK;
  }

  // Async: move every output the event pump counted into pendingOut_.
  HRESULT CollectAsync() {
    for (int guard = 0; guard < 16; ++guard) {
      {
        std::lock_guard<std::mutex> lk(events_->m);
        if (FAILED(events_->error)) return events_->error;
        if (events_->haveOutput == 0) return S_OK;
        --events_->haveOutput;
      }
      EncodedAu one;
      const HRESULT hr = PullOneOutput(one);
      if (hr == S_FALSE) {  // renegotiated: the same output is still pending
        std::lock_guard<std::mutex> lk(events_->m);
        ++events_->haveOutput;
        continue;
      }
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) continue;
      if (FAILED(hr)) return hr;
      ParkOutput(std::move(one));
    }
    return S_OK;
  }

  HRESULT EncodeAsync(IMFSample* s, const SourcePicture& src, LONGLONG time, EncodedAu& au) {
    // 1) Wait for an input credit, collecting outputs that arrive meanwhile
    // into pendingOut_ (each keeps its own input identity; one per call).
    ULONGLONG deadline = GetTickCount64() + 1000;
    for (;;) {
      HRESULT hr = CollectAsync();
      if (FAILED(hr)) return hr;
      {
        std::lock_guard<std::mutex> lk(events_->m);
        if (FAILED(events_->error)) return events_->error;
        if (events_->needInput > 0) {
          --events_->needInput;
          break;
        }
      }
      const DWORD left = MsLeft(deadline);
      if (!left) {
        WVD_LOGE("MF encoder '%s' asked for no input for 1 s — treating as failed",
                 friendly_.c_str());
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
      }
      WaitForSingleObject(events_->signal, left);
    }
    HRESULT hr = mft_->ProcessInput(inId_, s, 0);
    if (FAILED(hr)) return hr;
    RegisterPending(src, time);

    // 2) Low-latency MFTs answer every input with one output within a few ms.
    // Wait for it so a still desktop's last change is not left inside the
    // encoder; a pipelined MFT's late output is collected on the next call.
    deadline = GetTickCount64() + 100;
    for (;;) {
      {
        std::lock_guard<std::mutex> lk(events_->m);
        if (FAILED(events_->error)) return events_->error;
        if (events_->haveOutput > 0) break;
      }
      const DWORD left = MsLeft(deadline);
      if (!left) break;
      WaitForSingleObject(events_->signal, left);
    }
    hr = CollectAsync();
    if (FAILED(hr)) return hr;
    // Exactly one AU per call; whatever else completed waits its turn and
    // keeps ITS OWN input identity (resolved in AppendSample).
    return TakeOne(au);
  }

  EncoderInitDesc desc_;
  bool hardware_;
  EncoderCaps caps_;
  Vendor vendor_ = Vendor::Microsoft;
  Codec codec_ = Codec::HEVC;
  std::string friendly_;
  ComPtr<IMFActivate> activate_;
  ComPtr<IMFTransform> mft_;
  ComPtr<ICodecAPI> codecApi_;
  bool async_ = false;
  // Texture input: the MFT shares the session's D3D11 device.
  ComPtr<IMFDXGIDeviceManager> devMgr_;
  UINT resetToken_ = 0;
  bool gpu_ = false;
  bool gpuAllowed_ = true;
  bool producedOutput_ = false;
  // "No periodic keyframes" for MFTs: one hour of frames at 60 fps.
  static constexpr UINT32 kOnDemandGop = 216000;
  DWORD inId_ = 0, outId_ = 0;
  bool mftProvidesSamples_ = false;
  DWORD outBufSize_ = 0;
  std::shared_ptr<EventState> events_;
  EventPump* pump_ = nullptr;
  std::vector<uint8_t> csd_;
  // Inputs submitted with their sample time, waiting for the output that
  // carries the same time (encode thread only — see RegisterPending).
  std::deque<Pending> pending_;
  // Completed output AUs not yet handed to the caller (encode thread only).
  std::deque<EncodedAu> pendingOut_;
  LONGLONG frameIndex_ = 0;
  std::atomic<bool> forceKey_{true};  // first picture is a random access point (§6.6)
};

}  // namespace

MfProbeResult ProbeMfEncoders(const LUID& adapterLuid) {
  MfProbeResult r;
  auto hevc = EnumEncoders(MFVideoFormat_HEVC, true, adapterLuid);
  auto h264 = EnumEncoders(MFVideoFormat_H264, true, adapterLuid);
  r.hwHevc = !hevc.empty();
  r.hwH264 = !h264.empty();
  if (r.hwHevc)
    r.hwName = FriendlyName(hevc[0].Get());
  else if (r.hwH264)
    r.hwName = FriendlyName(h264[0].Get());
  r.swH264 = !EnumEncoders(MFVideoFormat_H264, false, adapterLuid).empty();
  return r;
}

std::unique_ptr<IVideoEncoder> CreateMfHardwareEncoder(const EncoderInitDesc& d) {
  auto e = std::make_unique<MfEncoder>(d, true);
  const HRESULT hr = e->Init();
  if (FAILED(hr)) {
    WVD_LOGW("no usable hardware MF encoder (hr=%s)", HresultStr(hr).c_str());
    return nullptr;
  }
  return e;
}

std::unique_ptr<IVideoEncoder> CreateMfSoftwareEncoder(const EncoderInitDesc& d) {
  auto e = std::make_unique<MfEncoder>(d, false);
  const HRESULT hr = e->Init();
  if (FAILED(hr)) {
    WVD_LOGW("software H.264 MFT unavailable (hr=%s; Windows N without the Media "
             "Feature Pack?)", HresultStr(hr).c_str());
    return nullptr;
  }
  return e;
}

}  // namespace wvd

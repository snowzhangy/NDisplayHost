#include "nvenc_encoder.h"
#include "dx.h"
#include "wvd/logging.h"
#include "wvd/time.h"
#include <algorithm>
#include <cstring>
#include <mutex>

#if defined(WVD_HAVE_NVENC) && WVD_HAVE_NVENC
#include <nvEncodeAPI.h>  // SDK declarations must remain outside namespace wvd
#endif

namespace wvd {
#if defined(WVD_HAVE_NVENC) && WVD_HAVE_NVENC
namespace {
struct NvencApi {
  HMODULE dll = nullptr;
  NV_ENCODE_API_FUNCTION_LIST functions{};
  bool ready = false;
  NvencApi() {
    dll = LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) return;
    using CreateFn = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
    auto create = reinterpret_cast<CreateFn>(GetProcAddress(dll, "NvEncodeAPICreateInstance"));
    using MaxVersionFn = NVENCSTATUS(NVENCAPI*)(uint32_t*);
    auto maxVersion = reinterpret_cast<MaxVersionFn>(GetProcAddress(dll, "NvEncodeAPIGetMaxSupportedVersion"));
    uint32_t version = 0;
    if (!create || !maxVersion || maxVersion(&version) != NV_ENC_SUCCESS ||
        version < ((NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION)) return;
    functions.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    ready = create(&functions) == NV_ENC_SUCCESS;
  }
  ~NvencApi() { if (dll) FreeLibrary(dll); }
};
NvencApi& Api() { static NvencApi api; return api; }

class NvencEncoder final : public IVideoEncoder {
 public:
  explicit NvencEncoder(const EncoderInitDesc& desc) : desc_(desc) {}
  ~NvencEncoder() override {
    if (!encoder_) return;
    auto& api = Api().functions;
    if (mapped_) api.nvEncUnmapInputResource(encoder_, mapped_);
    for (auto& in : inputs_)
      if (in.registered) api.nvEncUnregisterResource(encoder_, in.registered);
    if (bitstream_) api.nvEncDestroyBitstreamBuffer(encoder_, bitstream_);
    api.nvEncDestroyEncoder(encoder_);
  }
  const char* name() const override { return "nvenc-hevc"; }
  Vendor vendor() const override { return Vendor::NVIDIA; }
  uint8_t tier() const override { return 1; }
  Codec codec() const override { return Codec::HEVC; }
  const EncoderCaps& caps() const override { return caps_; }
  std::vector<uint8_t> codecPrivate() const override { return parameterSets_; }
  bool zeroCopy() const override { return shared_; }

  HRESULT SetBitrate(uint32_t kbps) override {
    if (!encoder_ || !kbps || kbps > UINT32_MAX / 1000) return E_INVALIDARG;
    NV_ENC_CONFIG config = config_;
    config.rcParams.averageBitRate = config.rcParams.maxBitRate = kbps * 1000;
    config.rcParams.vbvBufferSize = VbvBits(config.rcParams.averageBitRate);
    config.rcParams.vbvInitialDelay = config.rcParams.vbvBufferSize;
    NV_ENC_RECONFIGURE_PARAMS re{};
    re.version = NV_ENC_RECONFIGURE_PARAMS_VER;
    re.reInitEncodeParams = init_;
    re.reInitEncodeParams.encodeConfig = &config;
    // Rate change only: keep the reference chain, no IDR.
    if (Api().functions.nvEncReconfigureEncoder(encoder_, &re) != NV_ENC_SUCCESS) return E_FAIL;
    config_ = config;
    desc_.bitrateKbps = kbps;
    return S_OK;
  }

  HRESULT Init() {
    if (!Api().ready || !desc_.width || !desc_.height || (desc_.width & 1) ||
        (desc_.height & 1) || !desc_.fpsNum || !desc_.fpsDen ||
        !desc_.bitrateKbps || desc_.bitrateKbps > UINT32_MAX / 1000) return E_INVALIDARG;
    if (desc_.device) {
      // The pictures already live on this device. NVENC can only take them
      // if it is an NVIDIA adapter (on a hybrid laptop the display may hang
      // off the integrated GPU; that case belongs to the next tier).
      ComPtr<IDXGIDevice> dxgiDevice;
      ComPtr<IDXGIAdapter> adapter;
      DXGI_ADAPTER_DESC info{};
      if (FAILED(desc_.device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) ||
          FAILED(dxgiDevice->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&info)))
        return E_FAIL;
      if (info.VendorId != 0x10DE) return DXGI_ERROR_UNSUPPORTED;
      device_ = desc_.device;
      device_->GetImmediateContext(&context_);
      shared_ = true;
    }
    auto factory = shared_ ? nullptr : CreateDxgiFactory();
    if (!shared_ && !factory) return E_FAIL;
    bool found = shared_;
    for (const auto& adapter : shared_ ? std::vector<DxAdapterInfo>{}
                                       : EnumerateAdapters(factory.Get())) {
      const bool specified = desc_.adapterLuid.HighPart || desc_.adapterLuid.LowPart;
      if (specified && (adapter.luid.HighPart != desc_.adapterLuid.HighPart ||
                        adapter.luid.LowPart != desc_.adapterLuid.LowPart)) continue;
      ComPtr<IDXGIAdapter1> dxgi;
      if (FAILED(factory->EnumAdapters1(adapter.index, &dxgi))) continue;
      DXGI_ADAPTER_DESC1 info{};
      if (FAILED(dxgi->GetDesc1(&info)) || info.VendorId != 0x10DE) continue;
      if (FAILED(CreateDeviceForAdapter(factory.Get(), adapter.luid, device_, context_))) continue;
      found = true; break;
    }
    if (!found) return DXGI_ERROR_UNSUPPORTED;
    auto& api = Api().functions;
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session{};
    session.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    session.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    session.device = device_.Get(); session.apiVersion = NVENCAPI_VERSION;
    if (api.nvEncOpenEncodeSessionEx(&session, &encoder_) != NV_ENC_SUCCESS) return E_FAIL;
    NV_ENC_PRESET_CONFIG preset{};
    preset.version = NV_ENC_PRESET_CONFIG_VER; preset.presetCfg.version = NV_ENC_CONFIG_VER;
    const auto& presetGuid = desc_.preferQuality ? NV_ENC_PRESET_P4_GUID : NV_ENC_PRESET_P3_GUID;
    if (api.nvEncGetEncodePresetConfigEx(encoder_, NV_ENC_CODEC_HEVC_GUID, presetGuid,
                                         NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset) != NV_ENC_SUCCESS) return E_FAIL;
    auto config = preset.presetCfg;
    config.profileGUID = NV_ENC_HEVC_PROFILE_MAIN_GUID;
    // Keyframes on demand unless a period was requested (first picture and
    // recovery requests force an IDR).
    config.gopLength = desc_.gopSeconds
        ? (std::max)(1u, uint32_t(uint64_t(desc_.fpsNum) * desc_.gopSeconds / desc_.fpsDen))
        : NVENC_INFINITE_GOPLENGTH;
    config.frameIntervalP = 1;  // no reordered frames or delayed output
    config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    config.rcParams.averageBitRate = config.rcParams.maxBitRate = desc_.bitrateKbps * 1000;
    config.rcParams.vbvBufferSize = VbvBits(config.rcParams.averageBitRate);
    config.rcParams.vbvInitialDelay = config.rcParams.vbvBufferSize;
    config.rcParams.enableLookahead = 0;
    config.rcParams.zeroReorderDelay = 1;
    auto& hevc = config.encodeCodecConfig.hevcConfig;
    hevc.idrPeriod = config.gopLength;
    hevc.repeatSPSPPS = 1; hevc.chromaFormatIDC = 1;
    auto& vui = hevc.hevcVUIParameters;
    vui.videoSignalTypePresentFlag = 1; vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
    vui.videoFullRangeFlag = desc_.vui.fullRange;
    vui.colourDescriptionPresentFlag = 1;
    vui.colourPrimaries = static_cast<NV_ENC_VUI_COLOR_PRIMARIES>(desc_.vui.primaries);
    vui.transferCharacteristics = static_cast<NV_ENC_VUI_TRANSFER_CHARACTERISTIC>(desc_.vui.transfer);
    vui.colourMatrix = static_cast<NV_ENC_VUI_MATRIX_COEFFS>(desc_.vui.matrix);
    NV_ENC_INITIALIZE_PARAMS init{};
    init.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID = NV_ENC_CODEC_HEVC_GUID; init.presetGUID = presetGuid;
    init.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    init.encodeWidth = init.darWidth = init.maxEncodeWidth = desc_.width;
    init.encodeHeight = init.darHeight = init.maxEncodeHeight = desc_.height;
    init.frameRateNum = desc_.fpsNum; init.frameRateDen = desc_.fpsDen;
    init.enablePTD = 1; init.enableEncodeAsync = 0; init.encodeConfig = &config;
    if (api.nvEncInitializeEncoder(encoder_, &init) != NV_ENC_SUCCESS) return E_FAIL;
    // Kept for rate changes (nvEncReconfigureEncoder wants the full set).
    config_ = config;
    init_ = init;
    init_.encodeConfig = nullptr;

    NV_ENC_CREATE_BITSTREAM_BUFFER output{};
    output.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    if (api.nvEncCreateBitstreamBuffer(encoder_, &output) != NV_ENC_SUCCESS) return E_FAIL;
    bitstream_ = output.bitstreamBuffer;
    parameterSets_.resize(4096);
    uint32_t length = 0;
    NV_ENC_SEQUENCE_PARAM_PAYLOAD sequence{};
    sequence.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    sequence.inBufferSize = uint32_t(parameterSets_.size());
    sequence.spsppsBuffer = parameterSets_.data(); sequence.outSPSPPSPayloadSize = &length;
    if (api.nvEncGetSequenceParams(encoder_, &sequence) != NV_ENC_SUCCESS ||
        length > parameterSets_.size() || !length) return E_FAIL;
    parameterSets_.resize(length);
    caps_.hevc8 = true;
    caps_.maxFrameWidth = desc_.width; caps_.maxFrameHeight = desc_.height;
    return S_OK;
  }
  HRESULT Encode(const SourcePicture& source, EncodedAu& au) override {
    au = {}; au.qpcEncodeIn = QpcNow();
    auto& api = Api().functions;
    // Zero-copy: the session's NV12 texture is registered and encoded as is.
    // Otherwise (no shared device) the packed CPU picture is uploaded.
    ID3D11Texture2D* input = shared_ ? source.texture : nullptr;
    if (input) {
      D3D11_TEXTURE2D_DESC td{};
      input->GetDesc(&td);
      if (td.Width != desc_.width || td.Height != desc_.height || td.Format != DXGI_FORMAT_NV12)
        return E_INVALIDARG;
    } else {
      const uint8_t* nv12 = nullptr;
      size_t nv12Size = 0;
      HRESULT hr = source.Cpu(&nv12, &nv12Size);
      if (FAILED(hr)) return hr;
      if (nv12Size < size_t(desc_.width) * desc_.height * 3 / 2) return E_INVALIDARG;
      if (!upload_) {
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = desc_.width; texture.Height = desc_.height;
        texture.MipLevels = texture.ArraySize = texture.SampleDesc.Count = 1;
        texture.Format = DXGI_FORMAT_NV12; texture.Usage = D3D11_USAGE_DEFAULT;
        texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr = device_->CreateTexture2D(&texture, nullptr, &upload_);
        if (FAILED(hr)) return hr;
      }
      context_->UpdateSubresource(upload_.Get(), 0, nullptr, nv12, desc_.width, 0);
      context_->Flush();
      input = upload_.Get();
    }
    NV_ENC_REGISTERED_PTR registered = Register(input);
    if (!registered) return E_FAIL;
    NV_ENC_MAP_INPUT_RESOURCE mapping{};
    mapping.version = NV_ENC_MAP_INPUT_RESOURCE_VER; mapping.registeredResource = registered;
    if (api.nvEncMapInputResource(encoder_, &mapping) != NV_ENC_SUCCESS) return E_FAIL;
    mapped_ = mapping.mappedResource;
    auto unmap = [&] {
      const auto result = api.nvEncUnmapInputResource(encoder_, mapped_);
      if (result == NV_ENC_SUCCESS) mapped_ = nullptr;
      return result == NV_ENC_SUCCESS;
    };
    NV_ENC_PIC_PARAMS picture{};
    picture.version = NV_ENC_PIC_PARAMS_VER;
    picture.inputBuffer = mapped_; picture.bufferFmt = mapping.mappedBufferFmt;
    picture.inputWidth = desc_.width; picture.inputHeight = desc_.height;
    picture.outputBitstream = bitstream_; picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picture.inputTimeStamp = uint64_t(source.qpcCapture);
    picture.encodePicFlags = forceKey_ ? (NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS) : 0;
    if (api.nvEncEncodePicture(encoder_, &picture) != NV_ENC_SUCCESS) { unmap(); return E_FAIL; }
    NV_ENC_LOCK_BITSTREAM output{};
    output.version = NV_ENC_LOCK_BITSTREAM_VER; output.outputBitstream = bitstream_;
    if (api.nvEncLockBitstream(encoder_, &output) != NV_ENC_SUCCESS) { unmap(); return E_FAIL; }
    const auto* begin = static_cast<const uint8_t*>(output.bitstreamBufferPtr);
    au.bytes.assign(begin, begin + output.bitstreamSizeInBytes);
    au.key = output.pictureType == NV_ENC_PIC_TYPE_IDR;
    const auto unlocked = api.nvEncUnlockBitstream(encoder_, bitstream_);
    const bool unmapped = unmap();
    if (unlocked != NV_ENC_SUCCESS || !unmapped) return E_FAIL;
    forceKey_ = false;
    // NVENC's lock/unlock path is synchronous: these bytes are `source`.
    au.contentVer = source.contentVer; au.qpcCapture = source.qpcCapture;
    au.refinement = source.qualityBoost; au.depthValid = source.depthValid;
    au.qpcEncodeOut = QpcNow();
    return S_OK;
  }
  HRESULT ForceKeyframe() override { forceKey_ = true; return S_OK; }
 private:
  // ~1 frame of bits: every picture is small enough to arrive in time (§6.6).
  uint32_t VbvBits(uint32_t bitsPerSecond) const {
    return (std::max)(1u, uint32_t(uint64_t(bitsPerSecond) * desc_.fpsDen / desc_.fpsNum));
  }
  // Registration is per texture and costly, so it is cached. The session
  // rotates through a handful of textures; the cache holds a reference so a
  // registered texture cannot be freed underneath the encoder.
  NV_ENC_REGISTERED_PTR Register(ID3D11Texture2D* texture) {
    for (auto& in : inputs_)
      if (in.texture.Get() == texture) return in.registered;
    auto& api = Api().functions;
    if (inputs_.size() >= 8) {  // size change upstream: drop the stale set
      for (auto& in : inputs_) api.nvEncUnregisterResource(encoder_, in.registered);
      inputs_.clear();
    }
    NV_ENC_REGISTER_RESOURCE resource{};
    resource.version = NV_ENC_REGISTER_RESOURCE_VER;
    resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    resource.resourceToRegister = texture; resource.width = desc_.width;
    resource.height = desc_.height; resource.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
    resource.bufferUsage = NV_ENC_INPUT_IMAGE;
    if (api.nvEncRegisterResource(encoder_, &resource) != NV_ENC_SUCCESS) return nullptr;
    inputs_.push_back({texture, resource.registeredResource});
    return resource.registeredResource;
  }
  struct Input {
    ComPtr<ID3D11Texture2D> texture;
    NV_ENC_REGISTERED_PTR registered = nullptr;
  };
  EncoderInitDesc desc_;
  EncoderCaps caps_;
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  bool shared_ = false;              // encoding on the session's own device
  ComPtr<ID3D11Texture2D> upload_;   // CPU-input fallback target
  std::vector<Input> inputs_;
  NV_ENC_CONFIG config_{};
  NV_ENC_INITIALIZE_PARAMS init_{};
  void* encoder_ = nullptr;
  NV_ENC_INPUT_PTR mapped_ = nullptr;
  NV_ENC_OUTPUT_PTR bitstream_ = nullptr;
  std::vector<uint8_t> parameterSets_;
  bool forceKey_ = true;
};
}  // namespace
bool NvencModuleBuilt() { return true; }
bool NvencRuntimeAvailable() { return Api().ready; }
std::unique_ptr<IVideoEncoder> CreateNvencEncoder(const EncoderInitDesc& desc) {
  auto encoder = std::make_unique<NvencEncoder>(desc);
  if (FAILED(encoder->Init())) return nullptr;
  return encoder;
}
#else
bool NvencModuleBuilt() { return false; }
bool NvencRuntimeAvailable() { return false; }
std::unique_ptr<IVideoEncoder> CreateNvencEncoder(const EncoderInitDesc&) { return nullptr; }
#endif
}  // namespace wvd

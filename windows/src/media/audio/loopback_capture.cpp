#include "audio/loopback_capture.h"

#include <windows.h>
#include <audioclient.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propsys.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "wvd/logging.h"
#include "wvd/pacing.h"

using Microsoft::WRL::ComPtr;

namespace wvd {
namespace {

enum class SampleKind { Float32, Int16, Int24, Int32, Unsupported };

SampleKind KindOf(const WAVEFORMATEX* f) {
  bool isFloat = f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
  bool isPcm = f->wFormatTag == WAVE_FORMAT_PCM;
  if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22) {
    const auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
    isFloat = IsEqualGUID(x->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
    isPcm = IsEqualGUID(x->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) != 0;
  }
  if (isFloat && f->wBitsPerSample == 32) return SampleKind::Float32;
  if (isPcm && f->wBitsPerSample == 16) return SampleKind::Int16;
  if (isPcm && f->wBitsPerSample == 24) return SampleKind::Int24;
  if (isPcm && f->wBitsPerSample == 32) return SampleKind::Int32;  // includes 24-in-32 (MSB aligned)
  return SampleKind::Unsupported;
}

uint32_t ChannelMaskOf(const WAVEFORMATEX* f) {
  if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22)
    return reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->dwChannelMask;
  return 0;
}

inline float ToFloat(SampleKind k, const BYTE* p) {
  switch (k) {
    case SampleKind::Float32: {
      float v;
      std::memcpy(&v, p, 4);
      return v;
    }
    case SampleKind::Int16: {
      int16_t v;
      std::memcpy(&v, p, 2);
      return v / 32768.0f;
    }
    case SampleKind::Int24: {
      int32_t v = p[0] | (p[1] << 8) | (p[2] << 16);
      if (v & 0x800000) v -= 0x1000000;
      return v / 8388608.0f;
    }
    case SampleKind::Int32: {
      int32_t v;
      std::memcpy(&v, p, 4);
      return v / 2147483648.0f;
    }
    default:
      return 0;
  }
}

inline int16_t ToS16(float x) {
  float v = x * 32768.0f;
  if (!(v == v)) return 0;  // NaN
  if (v >= 32767.0f) return 32767;
  if (v <= -32768.0f) return -32768;
  return static_cast<int16_t>(std::lrintf(v));
}

// PKEY_Device_FriendlyName, defined here so no INITGUID / extra library is needed.
const PROPERTYKEY kDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

std::string FriendlyName(IMMDevice* device) {
  ComPtr<IPropertyStore> props;
  if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) return "default device";
  PROPVARIANT v;
  PropVariantInit(&v);
  std::string out = "default device";
  if (SUCCEEDED(props->GetValue(kDeviceFriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) {
    int n = WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, nullptr, 0, nullptr, nullptr);
    if (n > 1) {
      out.assign(static_cast<size_t>(n - 1), '\0');
      WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, out.data(), n, nullptr, nullptr);
    }
  }
  PropVariantClear(&v);
  return out;
}

// Raises a flag when the default console playback device changes (the user
// picked other speakers, or headphones were plugged in or removed).
class DefaultDeviceWatcher final : public IMMNotificationClient {
 public:
  explicit DefaultDeviceWatcher(std::atomic<bool>* changed) : changed_(changed) {}
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG n = --refs_;
    if (n == 0) delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
      *out = static_cast<IMMNotificationClient*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
    if (flow == eRender && role == eConsole) changed_->store(true);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
    return S_OK;
  }

 private:
  std::atomic<ULONG> refs_{1};
  std::atomic<bool>* changed_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Stereo fold-down
// ---------------------------------------------------------------------------

StereoDownmix StereoDownmix::For(uint32_t channels, uint32_t mask) {
  StereoDownmix m;
  m.channels = std::min(std::max(channels, 1u), kMaxChannels);
  if (m.channels == 1) {  // mono: the same signal on both sides
    m.left[0] = m.right[0] = 1.0f;
    return m;
  }
  if (mask == 0) {
    // Windows' default layouts (ksmedia.h KSAUDIO_SPEAKER_*) by channel count.
    static const uint32_t kDefault[9] = {0, SPEAKER_FRONT_CENTER, KSAUDIO_SPEAKER_STEREO,
                                         KSAUDIO_SPEAKER_STEREO | SPEAKER_FRONT_CENTER,
                                         KSAUDIO_SPEAKER_QUAD,
                                         KSAUDIO_SPEAKER_QUAD | SPEAKER_FRONT_CENTER,
                                         KSAUDIO_SPEAKER_5POINT1,
                                         KSAUDIO_SPEAKER_5POINT1_SURROUND | SPEAKER_BACK_CENTER,
                                         KSAUDIO_SPEAKER_7POINT1_SURROUND};
    mask = kDefault[std::min<uint32_t>(m.channels, 8)];
  }
  const float k = 0.70710678f;  // -3 dB
  uint32_t ch = 0;
  for (uint32_t bit = 0; bit < 32 && ch < m.channels; ++bit) {
    const uint32_t speaker = 1u << bit;
    if (!(mask & speaker)) continue;
    float l = 0, r = 0;
    switch (speaker) {
      case SPEAKER_FRONT_LEFT: l = 1; break;
      case SPEAKER_FRONT_RIGHT: r = 1; break;
      case SPEAKER_FRONT_CENTER: l = r = k; break;
      case SPEAKER_LOW_FREQUENCY: break;  // left out, as in the ITU fold-down
      case SPEAKER_BACK_LEFT: case SPEAKER_SIDE_LEFT: case SPEAKER_TOP_FRONT_LEFT: l = k; break;
      case SPEAKER_BACK_RIGHT: case SPEAKER_SIDE_RIGHT: case SPEAKER_TOP_FRONT_RIGHT: r = k; break;
      case SPEAKER_FRONT_LEFT_OF_CENTER: l = 0.92387953f; r = 0.38268343f; break;
      case SPEAKER_FRONT_RIGHT_OF_CENTER: l = 0.38268343f; r = 0.92387953f; break;
      case SPEAKER_TOP_BACK_LEFT: l = 0.5f; break;
      case SPEAKER_TOP_BACK_RIGHT: r = 0.5f; break;
      default: l = r = 0.5f; break;  // back/top centre and other centred speakers
    }
    m.left[ch] = l;
    m.right[ch] = r;
    ++ch;
  }
  return m;  // channels without a speaker bit keep gain 0
}

void StereoDownmix::Apply(const float* in, uint32_t frames, int16_t* out) const {
  for (uint32_t f = 0; f < frames; ++f) {
    const float* s = in + static_cast<size_t>(f) * channels;
    float l = 0, r = 0;
    for (uint32_t c = 0; c < channels; ++c) {
      l += s[c] * left[c];
      r += s[c] * right[c];
    }
    out[f * 2] = ToS16(l);
    out[f * 2 + 1] = ToS16(r);
  }
}

// ---------------------------------------------------------------------------
// Capture thread
// ---------------------------------------------------------------------------

void LoopbackCapture::Start(Sink sink) {
  Stop();
  sink_ = std::move(sink);
  stop_ = false;
  th_ = std::thread([this] { Run(); });
}

void LoopbackCapture::Stop() {
  stop_ = true;
  if (th_.joinable()) th_.join();
}

void LoopbackCapture::Run() {
  const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
      WVD_LOGW("audio: no audio device service (hr=0x%08lx); streaming without sound",
               static_cast<unsigned long>(hr));
    } else {
      auto* watcher = new DefaultDeviceWatcher(&defaultChanged_);
      const bool watching = SUCCEEDED(enumerator->RegisterEndpointNotificationCallback(watcher));
      bool reportedFailure = false;
      while (!stop_) {
        CaptureDefaultDevice(enumerator.Get(), reportedFailure);
        // Device gone, default changed or nothing to open: try again shortly
        // (at once for a new default device).
        for (int i = 0; i < 20 && !stop_ && !defaultChanged_; ++i) Sleep(50);
      }
      if (watching) enumerator->UnregisterEndpointNotificationCallback(watcher);
      watcher->Release();
    }
  }
  if (SUCCEEDED(coHr)) CoUninitialize();
}

bool LoopbackCapture::CaptureDefaultDevice(IMMDeviceEnumerator* enumerator, bool& reportedFailure) {
  defaultChanged_ = false;
  ComPtr<IMMDevice> device;
  ComPtr<IAudioClient> client;
  ComPtr<IAudioCaptureClient> capture;
  WAVEFORMATEX* mix = nullptr;
  HRESULT hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
  if (SUCCEEDED(hr))
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(client.GetAddressOf()));
  if (SUCCEEDED(hr)) hr = client->GetMixFormat(&mix);
  // 100 ms shared buffer; polled every 5 ms, so a late wake-up never overflows it.
  if (SUCCEEDED(hr))
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 1000000, 0, mix,
                            nullptr);
  if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&capture));
  const SampleKind kind = mix ? KindOf(mix) : SampleKind::Unsupported;
  if (SUCCEEDED(hr) && kind == SampleKind::Unsupported) hr = E_NOTIMPL;
  if (SUCCEEDED(hr) && (mix->nChannels == 0 || mix->nChannels > StereoDownmix::kMaxChannels ||
                       mix->nSamplesPerSec < 8000 || mix->nSamplesPerSec > 192000 ||
                       mix->nBlockAlign < mix->nChannels * (mix->wBitsPerSample / 8)))
    hr = E_INVALIDARG;
  if (SUCCEEDED(hr)) hr = client->Start();
  if (FAILED(hr)) {
    // Logged once per outage (no playback device, device in exclusive use…).
    if (!reportedFailure)
      WVD_LOGW("audio: loopback capture unavailable (hr=0x%08lx); retrying every second",
               static_cast<unsigned long>(hr));
    reportedFailure = true;
    if (mix) CoTaskMemFree(mix);
    return false;
  }
  reportedFailure = false;

  const uint32_t channels = mix->nChannels;
  const uint32_t rate = mix->nSamplesPerSec;
  const uint32_t block = mix->nBlockAlign;
  const uint32_t bytesPerSample = mix->wBitsPerSample / 8;
  const StereoDownmix downmix = StereoDownmix::For(channels, ChannelMaskOf(mix));
  WVD_LOGI("audio: loopback '%s' %u Hz, %u ch, %u-bit%s%s", FriendlyName(device.Get()).c_str(),
           rate, channels, mix->wBitsPerSample, kind == SampleKind::Float32 ? " float" : "",
           channels > 2 ? " (folded to stereo)" : "");
  CoTaskMemFree(mix);
  mix = nullptr;

  PrecisionSleeper sleeper;  // Sleep(5) would wake every 15.6 ms on a default timer
  std::vector<float> samples;
  std::vector<int16_t> stereo;
  while (!stop_ && !defaultChanged_) {
    sleeper.SleepMs(5);
    UINT32 packet = 0;
    hr = capture->GetNextPacketSize(&packet);
    while (SUCCEEDED(hr) && packet > 0 && !stop_) {
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
      if (FAILED(hr)) break;
      stereo.assign(static_cast<size_t>(frames) * 2, 0);
      if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
        samples.resize(static_cast<size_t>(frames) * channels);
        for (UINT32 f = 0; f < frames; ++f) {
          const BYTE* frame = data + static_cast<size_t>(f) * block;
          for (uint32_t c = 0; c < channels; ++c)
            samples[static_cast<size_t>(f) * channels + c] = ToFloat(kind, frame + c * bytesPerSample);
        }
        downmix.Apply(samples.data(), frames, stereo.data());
      }
      capture->ReleaseBuffer(frames);
      // The headset accepts at most 2048 frames per packet.
      for (UINT32 off = 0; off < frames; off += 2048)
        sink_(stereo.data() + static_cast<size_t>(off) * 2, std::min<UINT32>(2048, frames - off), rate);
      hr = capture->GetNextPacketSize(&packet);
    }
    if (FAILED(hr)) {
      // AUDCLNT_E_DEVICE_INVALIDATED: unplugged, disabled or its format changed.
      WVD_LOGW("audio: playback device lost (hr=0x%08lx); reopening", static_cast<unsigned long>(hr));
      break;
    }
  }
  if (defaultChanged_ && !stop_) WVD_LOGI("audio: default playback device changed; switching");
  client->Stop();
  return true;
}

}  // namespace wvd

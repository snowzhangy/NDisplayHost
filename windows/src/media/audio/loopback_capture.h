// System audio for the headset: WASAPI shared-mode loopback of the default
// render endpoint (plan §4.3 "Windows 10 fallback", which also works on
// Windows 11). Delivers interleaved 16-bit stereo at the device mix rate; the
// headset resamples to its output rate. The PC speakers keep playing: muting
// them needs the separate virtual audio endpoint driver (§4.3, deferred).
//
// The capture follows the default playback device: a change of default (or a
// device that goes away, such as unplugged headphones) reopens the new one,
// and a PC without a playback device is retried every second. 5.1 / 7.1 mixes
// are folded to stereo (StereoDownmix), so centre dialogue is kept.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

struct IMMDeviceEnumerator;

namespace wvd {

// Stereo fold-down gains for a device channel layout, ITU-R BS.775 style:
// front left/right at unity, centre and surround/back channels at -3 dB into
// their side (centre into both), LFE left out. Unity gains keep a stereo
// source on a 5.1/7.1 device at its level; a loud surround mix is clamped.
struct StereoDownmix {
  static constexpr uint32_t kMaxChannels = 32;
  uint32_t channels = 0;
  float left[kMaxChannels] = {};
  float right[kMaxChannels] = {};

  // `channelMask` is WAVEFORMATEXTENSIBLE::dwChannelMask (speaker bits in
  // channel order); 0 = the Windows default layout for that channel count.
  // Channels beyond the mask's speakers are left out. channels must be
  // 1...kMaxChannels.
  static StereoDownmix For(uint32_t channels, uint32_t channelMask);

  // in: frames x channels samples in [-1, 1]; out: frames x 2 int16.
  void Apply(const float* in, uint32_t frames, int16_t* out) const;
};

class LoopbackCapture {
 public:
  // `frames` interleaved stereo int16 frames captured at `sampleRate`.
  // Called on the capture thread; keep it short (enqueue and return).
  using Sink = std::function<void(const int16_t* stereo, uint32_t frames, uint32_t sampleRate)>;

  LoopbackCapture() = default;
  ~LoopbackCapture() { Stop(); }
  LoopbackCapture(const LoopbackCapture&) = delete;
  LoopbackCapture& operator=(const LoopbackCapture&) = delete;

  // Starts the capture thread (restarts it if running). Device errors are
  // logged there; video and input keep working without audio.
  void Start(Sink sink);
  void Stop();
  bool Running() const { return th_.joinable(); }

 private:
  void Run();
  // Captures the current default device until stop_, a default-device change
  // or a device error. False when the device could not be opened.
  bool CaptureDefaultDevice(IMMDeviceEnumerator* enumerator, bool& reportedFailure);

  Sink sink_;
  std::thread th_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> defaultChanged_{false};
};

}  // namespace wvd

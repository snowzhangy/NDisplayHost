#include "input_state.h"
#include "capture/dda_capture.h"
#include "Edid.h"
#include "encode/mf_encoder.h"
#include "audio/loopback_capture.h"
#include "screen_curtain.h"
#include "session/link_rate.h"
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
using namespace wvd;
std::vector<INPUT> sent;
bool failSend = false;
UINT WINAPI FakeSend(UINT count, LPINPUT inputs, int) {
  if (failSend) return 0;
  sent.insert(sent.end(), inputs, inputs + count); return count;
}
int main() {
  {
    DxOutputInfo primary, other, selected;
    primary.monitorName = LR"(\\.\DISPLAY49)";
    primary.monitorDevicePath = LR"(\\?\DISPLAY#same-monitor#instance)";
    primary.desktopCoords = {0, 0, 2040, 1230};
    other.monitorName = LR"(\\.\DISPLAY333)";
    other.monitorDevicePath = LR"(\\?\DISPLAY#other-monitor#instance)";
    other.desktopCoords = {2040, 0, 3960, 1080};
    const std::vector<DxOutputInfo> outputs{other, primary};
    CHECK(SelectOutput(outputs, nullptr, selected) && selected.monitorName == primary.monitorName);
    CHECK(SelectOutput(outputs, primary.monitorDevicePath.c_str(), selected) &&
          selected.monitorName == primary.monitorName); // same monitor after GDI renumbering
    CHECK(SelectOutput(outputs, L"display49", selected) && selected.monitorName == primary.monitorName);
    CHECK(!SelectOutput(outputs, LR"(\\.\DISPLAY33)", selected)); // never matches DISPLAY333
    CHECK(!SelectOutput(outputs, LR"(\\?\DISPLAY#removed-monitor#instance)", selected));
    CHECK(!SelectOutput({}, nullptr, selected));
  }
  {  // Screen off covers real screens only: never the WVD virtual monitor or other indirect displays.
    // The WVD driver reports an embedded-DisplayPort connector; its EDID id "WVD" gives it away.
    CHECK(IsVirtualMonitorPath(L"\\\\?\\DISPLAY#WVD0001#5&1a2b3c&0&UID256#{e6f07b5f}",
                               DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED));
    CHECK(IsVirtualMonitorPath(L"\\\\?\\display#wvd0001#1", DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI));
    CHECK(IsVirtualMonitorPath(L"\\\\?\\DISPLAY#ABC1234#1", DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL));
    CHECK(IsVirtualMonitorPath(nullptr, DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED));
    CHECK(!IsVirtualMonitorPath(L"\\\\?\\DISPLAY#SDC41A3#4&2f&0&UID8388688#{e6f07b5f}",
                                DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL));
    CHECK(!IsVirtualMonitorPath(nullptr, DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI));
  }
  {  // Link adaptation: short Wi-Fi pauses and keyframes keep the target; a slow link lowers it.
    const double frame = 1000.0 / 60;
    auto run = [&](LinkRate& r, double from, double to, auto waitAt) {
      uint32_t cuts = 0;
      for (double t = from; t < to; t += frame) {
        const uint32_t before = r.target(), k = r.OnPicture(t, waitAt(t));
        if (k && k < before) ++cuts;
      }
      return cuts;
    };
    LinkRate wifi(60000, 7500);  // a 60 ms pause every 1.5 s and a 40 ms keyframe burst every 2 s
    CHECK(run(wifi, 0, 20000, [&](double t) {
      return std::fmod(t, 1500) < frame ? 60.0 : std::fmod(t, 2000) < frame ? 40.0 : 0.0;
    }) == 0);
    CHECK(wifi.target() == 60000);
    LinkRate outage(60000, 7500);  // one 250 ms outage (the hold-back limit) is not a slow link
    CHECK(run(outage, 0, 5000, [&](double t) { return t >= 1000 && t < 1000 + frame ? 250.0 : 0.0; }) == 0);
    // Every picture waits 20 ms: the link cannot keep up. The first cut comes once 250 ms have
    // been waited (t = 200 ms); the next needs fresh evidence and comes 500 ms later.
    LinkRate slow(60000, 7500);
    CHECK(run(slow, 0, 1000, [](double) { return 20.0; }) == 2);
    CHECK(slow.target() == 33750 && slow.lastCutWaitedMs() >= 250);
    // Recovery: once the waits have left the 1 s window and 1.5 s after the last backlog
    // (t ~ 2480), one eighth of the ceiling per second, up to the ceiling.
    CHECK(run(slow, 1000, 2450, [](double) { return 0.0; }) == 0 && slow.target() == 33750);
    run(slow, 2450, 2600, [](double) { return 0.0; });
    CHECK(slow.target() == 41250);
    run(slow, 2600, 9000, [](double) { return 0.0; });
    CHECK(slow.target() == 60000);
    // A still desktop sends no pictures; the target climbs anyway.
    LinkRate still(60000, 7500);
    run(still, 0, 500, [](double) { return 20.0; });
    CHECK(still.target() == 45000);
    CHECK(still.OnIdle(1500) == 0 && still.OnIdle(2000) == 52500 && still.OnIdle(2500) == 0 &&
          still.OnIdle(3000) == 60000);
    // Never below the floor.
    LinkRate floor(60000, 40000);
    run(floor, 0, 10000, [](double) { return 30.0; });
    CHECK(floor.target() == 40000);
  }
  {  // Loopback audio: any device layout folds to stereo without losing the centre.
    int16_t out[4] = {};
    const float stereoIn[4] = {0.5f, -0.25f, 1.0f, -1.0f};
    StereoDownmix::For(2, 0).Apply(stereoIn, 2, out);  // stereo passes unchanged (full scale clamps)
    CHECK(out[0] == 16384 && out[1] == -8192 && out[2] == 32767 && out[3] == -32768);
    const float monoIn[1] = {0.5f};
    StereoDownmix::For(1, SPEAKER_FRONT_CENTER).Apply(monoIn, 1, out);
    CHECK(out[0] == 16384 && out[1] == 16384);
    // 5.1 (FL FR FC LFE BL BR): dialogue in the centre reaches both sides at -3 dB, LFE is left out.
    const auto m51 = StereoDownmix::For(6, KSAUDIO_SPEAKER_5POINT1);
    const float centre[6] = {0, 0, 0.5f, 0, 0, 0}, lfe[6] = {0, 0, 0, 0.9f, 0, 0};
    const float backLeft[6] = {0, 0, 0, 0, 0.5f, 0};
    m51.Apply(centre, 1, out); CHECK(out[0] == 11585 && out[1] == 11585);
    m51.Apply(lfe, 1, out); CHECK(out[0] == 0 && out[1] == 0);
    m51.Apply(backLeft, 1, out); CHECK(out[0] == 11585 && out[1] == 0);
    // No channel mask: Windows' default 7.1 order; side right is channel 7.
    const float sideRight[8] = {0, 0, 0, 0, 0, 0, 0, 0.5f};
    StereoDownmix::For(8, 0).Apply(sideRight, 1, out); CHECK(out[0] == 0 && out[1] == 11585);
    // A mask with fewer speakers than channels: the extra channel is not mixed in.
    const float extra[3] = {0, 0, 1.0f};
    StereoDownmix::For(3, KSAUDIO_SPEAKER_STEREO).Apply(extra, 1, out); CHECK(out[0] == 0 && out[1] == 0);
    // A loud surround mix clamps instead of wrapping around.
    const float loud[6] = {1.0f, 1.0f, 1.0f, 0, 1.0f, 1.0f};
    m51.Apply(loud, 1, out); CHECK(out[0] == 32767 && out[1] == 32767);
    const float nan[2] = {std::nanf(""), -0.0f};
    StereoDownmix::For(2, 0).Apply(nan, 1, out); CHECK(out[0] == 0 && out[1] == 0);
  }
  CHECK(MfFrameBufferBytes(80000, 60, 1) == 166667);
  CHECK(MfFrameBufferBytes(60000, 60, 1) == 125000);
  CHECK(MfFrameBufferBytes(80000, 60000, 1001) == 166834);
  CHECK(MfFrameBufferBytes(1, 240, 1) == 1);
  CHECK(MfFrameBufferBytes(1, 0, 1) == 0);
  CHECK(MfFrameBufferBytes(UINT32_MAX, 1, UINT32_MAX) == UINT32_MAX);
  InputGate gate;
  CHECK(!gate.Accept(gate.generation()));
  gate.Arm(); CHECK(!gate.Accept(gate.generation()));  // starts suspended
  gate.Suspend(false); const auto generation = gate.generation();
  CHECK(gate.Accept(generation)); CHECK(!gate.Accept(generation + 1));
  gate.Suspend(true); CHECK(!gate.Accept(generation));
  gate.Suspend(false); CHECK(gate.Accept(generation));
  gate.Disarm(); CHECK(gate.generation() > generation);
  gate.Arm(); CHECK(!gate.Accept(generation)); CHECK(gate.Accept(gate.generation()));
  CHECK(HidUsageToScanCode(0x04).code == 0x1E);
  CHECK(HidUsageToScanCode(0xE4).extended);
  CHECK(!HidUsageToScanCode(0xFFFF).valid);
  InputInjector injector(FakeSend);
  InputEvent key{}; key.kind = uint8_t(InputKind::Key); key.code = 0xE0;
  failSend = true; injector.InjectEvent(key); CHECK(injector.heldCount() == 0);
  failSend = false; injector.InjectEvent(key); CHECK(injector.heldCount() == 1);
  failSend = true; injector.ReleaseAllHeld(); CHECK(injector.heldCount() == 1);
  failSend = false; injector.ReleaseAllHeld(); CHECK(injector.heldCount() == 0);
  CHECK((sent.back().ki.dwFlags & KEYEVENTF_KEYUP) != 0);
  InputEvent button{}; button.kind = uint8_t(InputKind::Button); button.code = 3;
  injector.InjectEvent(button); CHECK(injector.heldCount() == 2);
  CHECK((sent.back().mi.dwFlags & (MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_RIGHTDOWN)) ==
        (MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_RIGHTDOWN));
  injector.ReleaseAllHeld(); CHECK(injector.heldCount() == 0);

  DXGI_OUTDUPL_POINTER_SHAPE_INFO shape{};
  shape.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
  shape.Width = 4; shape.Height = 2; shape.Pitch = 1;
  const uint8_t mask[] = {0x30, 0x50};  // black, white, transparent, invert
  CursorShapeData cursor;
  CHECK(ConvertDdaPointerShape(shape, mask, sizeof(mask), cursor));
  CHECK(cursor.height == 1 && cursor.pixels.size() == 16);
  CHECK(cursor.pixels[3] == 255 && cursor.pixels[0] == 0);
  CHECK(cursor.pixels[4] == 255 && cursor.pixels[7] == 255);
  CHECK(cursor.pixels[11] == 0 && cursor.pixels[12] == 128);
  CHECK(!ConvertDdaPointerShape(shape, mask, 1, cursor));
  shape.Height = 3; CHECK(!ConvertDdaPointerShape(shape, mask, 2, cursor));
  shape.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR;
  shape.Height = 1; shape.Pitch = 16;
  const uint32_t masked[] = {0x00000000, 0x0000FF00, 0xFF000000, 0xFFFFFFFF};
  CHECK(ConvertDdaPointerShape(shape, reinterpret_cast<const uint8_t*>(masked), sizeof(masked), cursor));
  CHECK(cursor.pixels[3] == 255 && cursor.pixels[0] == 0);  // opaque BLACK
  CHECK(cursor.pixels[5] == 255 && cursor.pixels[7] == 255);
  CHECK(cursor.pixels[11] == 0 && cursor.pixels[12] == 128);
  shape.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
  const uint32_t color[] = {0xFF000000, 0x800000FF, 0, 0xFFFFFFFF};
  CHECK(ConvertDdaPointerShape(shape, reinterpret_cast<const uint8_t*>(color), sizeof(color), cursor));
  CHECK(cursor.pixels[3] == 255 && cursor.pixels[0] == 0);
  CHECK(cursor.pixels[4] == 128 && cursor.pixels[7] == 128);
  const auto hash = cursor.hash;
  shape.Width = 2; shape.Height = 2; shape.Pitch = 8;
  CHECK(ConvertDdaPointerShape(shape, reinterpret_cast<const uint8_t*>(color), sizeof(color), cursor));
  CHECK(hash != cursor.hash);  // same pixels, different geometry

  WvdEdid edid;
  WvdBuildEdid(edid, kWvdReferenceModes[0], 0x12345678, 60, kWvdMonitorName);
  unsigned sum = 0; for (auto byte : edid.data) sum += byte;
  CHECK(sum % 256 == 0 && edid.data[18] == 1 && edid.data[19] == 4);
  const auto* d = edid.data + 54;
  const unsigned width = d[2] | ((d[4] >> 4) << 8);
  const unsigned height = d[5] | ((d[7] >> 4) << 8);
  CHECK(width == 3840 && height == 2160);
  const unsigned totalW = width + d[3] + ((d[4] & 15) << 8);
  const unsigned totalH = height + d[6] + ((d[7] & 15) << 8);
  CHECK((d[0] | (d[1] << 8)) * 10000ull == totalW * totalH * 60ull);
  CHECK(edid.data[75] == 0xFC && edid.data[38] == 1 && edid.data[53] == 1);
  CHECK(d[12] + ((d[14] >> 4) << 8) == 1625);
  // Every advertised mode is something the headset accepts (even, <= 8192)
  // and the size menu has 16:9, 21:9 and 32:9 choices.
  bool wide = false, ultrawide = false;
  for (unsigned i = 0; i < kWvdReferenceModeCount; ++i) {
    const WvdMode& m = kWvdReferenceModes[i];
    CHECK(m.width % 2 == 0 && m.height % 2 == 0 && m.width <= 8192 && m.height <= 8192);
    CHECK(m.vSync >= 60 && WvdSignalTiming(m).pixelRate < 0xFFFFFFFFull);
    wide = wide || (m.width * 9 > m.height * 16 + m.height && m.width * 9 < m.height * 32 - m.height);
    ultrawide = ultrawide || m.width * 9 == m.height * 32;
  }
  CHECK(kWvdReferenceModes[0].width * 9 == kWvdReferenceModes[0].height * 16);
  CHECK(wide && ultrawide);
  puts("input, cursor and EDID regressions passed");
}

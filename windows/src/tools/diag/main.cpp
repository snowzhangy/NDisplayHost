// wvd-diag — bring-up and §8 measurement helpers.
//
//   wvd-diag adapters                DXGI adapters + outputs
//   wvd-diag topology                CCD targets (find the WVD monitor)
//   wvd-diag caps                    encoder capability matrix (§4.6)
//   wvd-diag modes                   sizes the headset's size menu would offer
//   wvd-diag capture [--monitor N] [--seconds S] [--write-nv12 F]
//                                    DDA smoke: fps, dirty stats, pointer state
//   wvd-diag sink --port P           receive WVD packets (dev headset stub)
//   wvd-diag owner-ping              IPC to wvd-owner
//   wvd-diag coords-selftest         §4.4 transform checks
//   wvd-diag curtain-test            --screen-off curtain stays out of the stream
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "capture/dda_capture.h"
#include "display_modes.h"
#include "dx.h"
#include "encode/d3d12ve_probe.h"
#include "encode/iencoder.h"
#include "preprocess/nv12_convert.h"
#include "screen_curtain.h"
#include "wvd/coords.h"
#include "wvd/ipc.h"
#include "wvd/logging.h"
#include "wvd/protocol.h"
#include "wvd/time.h"
#include "wvd/transport.h"

// owner/topology.h (compiled into diag directly)
#include "../../owner/topology.h"

using namespace wvd;

namespace {

int CmdAdapters() {
  auto f = CreateDxgiFactory();
  if (!f) return 1;
  printf("== DXGI adapters ==\n");
  for (auto& a : EnumerateAdapters(f.Get())) {
    auto w = WideToUtf8(a.name.c_str());
    printf("  [%u] %-40s luid=%lu:%lu vram=%lluMB%s\n", a.index, w.c_str(),
           a.luid.HighPart, a.luid.LowPart, (unsigned long long)a.vramMB,
           a.isSoftware ? " (software)" : "");
  }
  printf("== outputs ==\n");
  for (auto& o : EnumerateOutputs(f.Get())) {
    auto w = WideToUtf8(o.monitorName.c_str());
    const auto devicePath = WideToUtf8(o.monitorDevicePath.c_str());
    printf("  %-16s rect=(%ld,%ld %ldx%ld) adapterLuid=%lu:%lu devicePath=\"%s\"\n", w.c_str(),
           o.desktopCoords.left, o.desktopCoords.top,
           o.desktopCoords.right - o.desktopCoords.left,
           o.desktopCoords.bottom - o.desktopCoords.top, o.adapterLuid.HighPart,
           o.adapterLuid.LowPart, devicePath.c_str());
  }
  return 0;
}

// Read-only: the per-output list wvd-media advertises (DisplayModes).
int CmdModes() {
  auto f = CreateDxgiFactory();
  if (!f) return 1;
  int outputs = 0;
  for (auto& o : EnumerateOutputs(f.Get())) {
    ++outputs;
    const auto name = WideToUtf8(o.monitorName.c_str());
    const auto modes = ListDisplayModes(o.monitorName);
    printf("== %s: %zu size(s) ==\n", name.c_str(), modes.size());
    for (const auto& m : modes) {
      const bool scaled = uint64_t(m.width) * m.height > 3840ull * 2160ull;
      printf("  %5ux%-5u @ %3u Hz%s%s%s\n", m.width, m.height, m.refreshHz,
             m.current ? "  current" : "", m.native ? "  native" : "",
             scaled ? "  (scaled to the 3840x2160 encode budget)" : "");
    }
  }
  if (!outputs) printf("no outputs\n");
  return outputs ? 0 : 1;
}

int CmdTopology(int argc, char** argv) {
  bool all = argc >= 3 && !strcmp(argv[2], "--all");
  std::vector<wvd::CcdMonitor> mons;
  std::wstring err;
  if (!wvd::CcdEnumerate(mons, err)) {
    printf("CCD failed: %s\n", WideToUtf8(err.c_str()).c_str());
    return 1;
  }
  for (auto& m : mons) {
    if (!all && !m.activePath && !m.attachedToTarget) continue;
    auto fn = WideToUtf8(m.friendly.c_str());
    printf("  path=%-3u target=%-3u %-38s %-6s %-8s %6ld,%-6ld %5ux%-5u%s\n",
           m.pathId, m.targetId, fn.c_str(), m.activePath ? "active" : "idle",
           m.attachedToTarget ? "attached" : "detached", m.x, m.y, m.width, m.height,
           m.primary ? " PRIMARY" : "");
  }
  return 0;
}

int CmdCaps() {
  EncoderInitDesc d;
  auto cands = ProbeEncoderCandidates(d);
  printf("== encoder capability matrix (§4.6) ==\n");
  printf("  %-10s %-4s %-6s %s\n", "name", "tier", "vendor", "status / caps");
  for (auto& c : cands) {
    const char* vend = "?";
    switch (c.vendor) {
      case Vendor::NVIDIA: vend = "NVIDIA"; break;
      case Vendor::AMD: vend = "AMD"; break;
      case Vendor::Intel: vend = "Intel"; break;
      case Vendor::Qualcomm: vend = "QCOM"; break;
      case Vendor::Microsoft: vend = "MS"; break;
      case Vendor::Test: vend = "test"; break;
    }
    printf("  %-10s t%u    %-6s %s%s%s%u\n", c.name.c_str(), c.tier, vend,
           c.available ? "AVAILABLE " : "n/a      ", c.reason.c_str(),
           c.caps.maxFrameWidth ? "  max=" : "", c.caps.maxFrameWidth);
  }
  return 0;
}

int CmdCapture(int argc, char** argv) {
  std::string monitor;
  int seconds = 5;
  std::string nv12File;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--monitor" && i + 1 < argc) monitor = argv[++i];
    else if (a == "--seconds" && i + 1 < argc) seconds = atoi(argv[++i]);
    else if (a == "--write-nv12" && i + 1 < argc) nv12File = argv[++i];
  }

  auto f = CreateDxgiFactory();
  if (!f) return 1;
  DxOutputInfo oi;
  if (!FindOutput(f.Get(), monitor.empty() ? nullptr : Utf8ToWide(monitor.c_str()).c_str(),
                  oi)) {
    printf("output not found\n");
    return 1;
  }
  auto wn = WideToUtf8(oi.monitorName.c_str());
  printf("capturing %s\n", wn.c_str());

  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  if (FAILED(CreateDeviceForAdapter(f.Get(), oi.adapterLuid, dev, ctx))) return 1;

  FrameRing ring;
  Nv12Converter conv;
  if (FAILED(conv.Init(dev.Get(), ctx.Get()))) return 1;

  // Need the IDXGIOutput interface object on this adapter.
  ComPtr<IDXGIOutput> output;
  for (UINT ai = 0;; ++ai) {
    ComPtr<IDXGIAdapter1> a;
    if (f->EnumAdapters1(ai, &a) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 ad{};
    a->GetDesc1(&ad);
    if (ad.AdapterLuid.LowPart != oi.adapterLuid.LowPart ||
        ad.AdapterLuid.HighPart != oi.adapterLuid.HighPart)
      continue;
    for (UINT ox = 0;; ++ox) {
      ComPtr<IDXGIOutput> o;
      if (a->EnumOutputs(ox, &o) == DXGI_ERROR_NOT_FOUND) break;
      DXGI_OUTPUT_DESC od{};
      o->GetDesc(&od);
      if (od.DeviceName == oi.monitorName) {
        output = o;
        break;
      }
    }
    if (output) break;
  }
  if (!output) return 1;

  DdaCapture dda;
  // Ring is sized from the DUPLICATION, not from desktop coordinates: coords
  // are DPI-virtualised when the process is not per-monitor DPI aware, while
  // the duplication always delivers physical pixels.
  HRESULT hr = dda.Start(DdaCapture::InitDesc{output, dev, ctx, nullptr});
  if (FAILED(hr)) {
    printf("DDA start failed hr=%s (interactive desktop required)\n",
           HresultStr(hr).c_str());
    return 1;
  }
  UINT w = dda.width(), h = dda.height();
  if (FAILED(ring.Init(dev.Get(), w, h, 3))) return 1;
  dda.SetRing(&ring);
  const LONG dw = LONG(oi.desktopCoords.right - oi.desktopCoords.left);
  const LONG dh = LONG(oi.desktopCoords.bottom - oi.desktopCoords.top);
  if (w != UINT(dw) || h != UINT(dh))
    printf("NOTE: desktop coords %ldx%ld != capture %ux%u (DPI scaling)\n", dw, dh, w, h);

  uint64_t frames = 0, contentChanges = 0, ptrFrames = 0, published = 0;
  uint64_t dirtyRectCount = 0, moveRectCount = 0, shapeUpdates = 0;
  auto endQpc = QpcNow() + MsToQpc(seconds * 1000.0);
  std::vector<uint8_t> nv12;
  bool wrote = false;
  while (QpcNow() < endQpc) {
    CapturedFrame f2;
    hr = dda.Acquire(250, f2);
    if (hr == S_OK) {
      ++frames;
      ring.Publish(f2.ringSlot, ++published);  // full-frame copy is in the slot
      if (f2.hadDirtyRects) ++contentChanges;
      dirtyRectCount += f2.dirtyRects.size();
      moveRectCount += f2.moves.size();
      if (f2.pointer.hasShape) ++shapeUpdates;
      if (f2.pointer.visible) ++ptrFrames;
      if (!nv12File.empty() && !wrote) {
        UINT slot = f2.ringSlot;
        const int64_t t0 = QpcNow();
        HRESULT chr = conv.Convert(ring.SlotSrv(slot), ring.width(), ring.height(),
                                   ChromaFilter::TextAware, nv12);
        const int64_t t1 = QpcNow();
        printf("NV12 convert (compute + 1x readback): %.2f ms  hr=%s\n", QpcDeltaMs(t0, t1),
               HresultStr(chr).c_str());
        // Second call: resources already cached — shows the steady-state cost.
        const int64_t t2 = QpcNow();
        conv.Convert(ring.SlotSrv(slot), ring.width(), ring.height(), ChromaFilter::TextAware,
                     nv12);
        printf("NV12 convert (steady state):          %.2f ms\n",
               QpcDeltaMs(t2, QpcNow()));
        // Plain readback of the same pixels, no compute shader, for comparison.
        {
          D3D11_TEXTURE2D_DESC sd{};
          ring.SlotTexture(slot)->GetDesc(&sd);
          sd.Usage = D3D11_USAGE_STAGING;
          sd.BindFlags = 0;
          sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
          ComPtr<ID3D11Texture2D> st;
          ComPtr<ID3D11Device> d2;
          ring.SlotTexture(slot)->GetDevice(&d2);
          ComPtr<ID3D11DeviceContext> c2;
          d2->GetImmediateContext(&c2);
          if (SUCCEEDED(d2->CreateTexture2D(&sd, nullptr, &st))) {
            const int64_t t3 = QpcNow();
            c2->CopyResource(st.Get(), ring.SlotTexture(slot));
            D3D11_MAPPED_SUBRESOURCE mp{};
            c2->Map(st.Get(), 0, D3D11_MAP_READ, 0, &mp);
            volatile uint8_t sink = 0;
            sink = ((uint8_t*)mp.pData)[0];
            c2->Unmap(st.Get(), 0);
            printf("plain BGRA readback (%zu MB):       %.2f ms  (%d)\n",
                   (size_t(sd.Width) * sd.Height * 4) / (1024 * 1024),
                   QpcDeltaMs(t3, QpcNow()), int(sink));
          }
        }
        if (SUCCEEDED(chr)) {
          FILE* fp = fopen(nv12File.c_str(), "wb");
          if (fp) {
            fwrite(nv12.data(), 1, nv12.size(), fp);
            fclose(fp);
            printf("wrote %zu bytes NV12 (%ux%u) to %s\n", nv12.size(), ring.width(),
                   ring.height(), nv12File.c_str());
            wrote = true;
          }
        }
      }
    } else if (hr == DXGI_ERROR_ACCESS_LOST) {
      printf("ACCESS_LOST — restarting duplication\n");
      dda.Stop();
      if (SUCCEEDED(dda.Start(DdaCapture::InitDesc{output, dev, ctx, nullptr}))) {
        ring.Deinit();
        ring.Init(dev.Get(), dda.width(), dda.height(), 3);
        dda.SetRing(&ring);
      }
    } else if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
      // Static desktop — no new frame. Expected.
    } else if (FAILED(hr)) {
      printf("Acquire failed hr=%s\n", HresultStr(hr).c_str());
      break;
    }
  }
  double secs = double(seconds);
  printf("frames=%llu contentChanges=%llu pointerVisible=%llu  (%.1f fps capture)\n",
         (unsigned long long)frames, (unsigned long long)contentChanges,
         (unsigned long long)ptrFrames, frames / secs);
  printf("dirtyRects=%llu moveRects=%llu pointerShapes=%llu\n",
         (unsigned long long)dirtyRectCount, (unsigned long long)moveRectCount,
         (unsigned long long)shapeUpdates);
  auto rs = ring.GetStats();
  printf("ring: written=%llu consumed=%llu depth<=%zu\n", (unsigned long long)rs.written,
         (unsigned long long)rs.consumed, rs.depth);
  return 0;
}

int CmdSink(int argc, char** argv) {
  std::string host = "127.0.0.1";
  int port = 50001;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--host" && i + 1 < argc) host = argv[++i];
    else if (a == "--port" && i + 1 < argc) port = atoi(argv[++i]);
  }
  WinsockGuard ws;
  TcpChannel ch;
  if (!ch.Connect(host.c_str(), uint16_t(port), 5000)) {
    printf("connect %s:%d failed\n", host.c_str(), port);
    return 1;
  }
  printf("connected; sending Hello\n");
  PacketWriter hello(PacketKind::Hello);
  HelloPayload hp{};
  hp.protocolVersion = kWireVersion;
  hp.maxDecodedPixelsPerSec = 750'000'000;
  memcpy(hp.deviceName, "wvd-diag-sink", 13);
  hello.Append(hp);
  auto hb = hello.Take();
  ch.Send(hb.data(), hb.size());

  uint64_t counts[64] = {};
  uint64_t mediaBytes = 0;
  int64_t lastReport = QpcNow();
  uint64_t lastTotal = 0;
  for (;;) {
    auto pkt = ch.RecvPacket(2000);
    if (!pkt) {
      printf("peer gone / timeout\n");
      break;
    }
    auto pv = PacketView::Parse(pkt->data(), pkt->size());
    if (!pv) continue;
    uint16_t k = pv->header().kind;
    if (k < 64) ++counts[k];
    if (pv->kind() == PacketKind::Media) {
      auto mh = pv->Get<MediaPayloadHeader>();
      if (mh) mediaBytes += mh->dataSize;
    }
    if (pv->kind() == PacketKind::CodecConfig) {
      auto cc = pv->Get<CodecConfigPayload>();
      if (cc)
        printf("CodecConfig gen=%llu %ux%u %u/%u bitrate=%ukbps vui(p=%u t=%u m=%u full=%u)\n",
               (unsigned long long)cc->codecGen, cc->width, cc->height, cc->fpsNum,
               cc->fpsDen, cc->bitrateKbps, cc->vui.primaries, cc->vui.transfer,
               cc->vui.matrix, cc->vui.fullRange);
    }
    if (pv->kind() == PacketKind::MonitorLayout) {
      auto ml = pv->Get<MonitorLayoutPayload>();
      if (ml) {
        auto mons = pv->ArrayAt<MonitorDesc>(sizeof(MonitorLayoutPayload), ml->count);
        printf("MonitorLayout gen=%llu count=%u\n", (unsigned long long)ml->layoutGen,
               ml->count);
        for (auto& m : mons)
          printf("  %-16s %6d,%-6d %5ux%-5u%s\n", m.id, m.x, m.y, m.width, m.height,
                 m.primary ? " primary" : "");
      }
    }
    int64_t now = QpcNow();
    if (now - lastReport > MsToQpc(1000)) {
      uint64_t total = 0;
      for (auto c : counts) total += c;
      printf("pkt/s=%llu media=%llu curShape=%llu curPos=%llu mediaBytes=%.1fMB\n",
             (total - lastTotal), counts[uint16_t(PacketKind::Media)],
             counts[uint16_t(PacketKind::CursorShape)],
             counts[uint16_t(PacketKind::CursorPos)], mediaBytes / 1e6);
      lastReport = now;
      lastTotal = total;
    }
  }
  return 0;
}

int CmdOwnerPing() {
  PipeClient c;
  if (!c.Open(L"\\\\.\\pipe\\wvd.owner", 1500)) {
    printf("owner pipe not reachable (service running?)\n");
    return 1;
  }
  IpcHeader hdr{};
  hdr.cmd = CmdPing;
  PacketWriter w(PacketKind::IpcCommand);
  w.Append(hdr);
  std::vector<uint8_t> resp;
  if (!c.Request(w.Take(), resp, 1500)) {
    printf("no response\n");
    return 1;
  }
  auto pv = PacketView::Parse(resp.data(), resp.size());
  if (!pv) return 1;
  auto st = pv->Get<IpcStatusResponse>();
  if (!st) return 1;
  printf("owner ok: state=%u sessionGen=%llu layoutGen=%llu\n", st->state,
         (unsigned long long)st->sessionGen, (unsigned long long)st->layoutGen);
  return 0;
}

int CmdCoordsSelftest() {
  // Negative origins + rotation + multi-monitor (§4.4).
  std::vector<MonitorDesc> mons(2);
  auto& a = mons[0];
  strcpy_s(a.id, "\\\\.\\DISPLAY1");
  a.x = -1920; a.y = 0; a.width = 1920; a.height = 1080; a.rotation = 0;
  auto& b = mons[1];
  strcpy_s(b.id, "\\\\.\\DISPLAY2");
  b.x = 0; b.y = 0; b.width = 2560; b.height = 1440; b.rotation = 0;
  LayoutMapper lm(mons);
  int fails = 0;
  auto check = [&](const char* what, bool ok) {
    printf("  %-40s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++fails;
  };
  auto p1 = lm.UvToDesktop(0, 0.f, 0.f);
  check("left monitor uv(0,0) -> (-1920,0)", p1 && p1->first == -1920 && p1->second == 0);
  auto p2 = lm.UvToDesktop(0, 1.f, 1.f);
  check("left monitor uv(1,1) -> (-1,-1)", p2 && p2->first == -1 && p2->second == 1079);
  auto p3 = lm.UvToDesktop(1, 0.5f, 0.5f);
  // round-to-nearest of 0.5·(2560−1) = 1279.5 → 1280 (719.5 → 720)
  check("right monitor center -> (1280,720)", p3 && p3->first == 1280 && p3->second == 720);
  auto s1 = lm.DesktopToSendInput(-1920, 0);
  check("virtualdesk norm of (-1920,0) == (0,0)", s1 && s1->first == 0 && s1->second == 0);
  auto s2 = lm.DesktopToSendInput(2559, 1439);
  check("virtualdesk norm of (2559,1439) == (65535,65535)",
        s2 && s2->first == 65535 && s2->second == 65535);
  // 90° rotation surface mapping stays within bounds:
  mons[1].rotation = 90;
  lm.Update(mons);
  auto p4 = lm.UvToDesktop(1, 1.f, 0.f);
  check("rot90 uv inside bounds", p4 && p4->first >= 0 && p4->first < 2560 &&
                                    p4->second >= 0 && p4->second < 1440);
  printf(fails ? "COORDS SELFTEST FAILED (%d)\n" : "coords selftest passed\n", fails);
  return fails ? 1 : 0;
}

// ---- curtain-test: is the --screen-off curtain hidden from Desktop Duplication?

void PumpFor(int ms) {
  const ULONGLONG end = GetTickCount64() + ms;
  while (GetTickCount64() < end) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    Sleep(10);
  }
}

// One duplicated frame of `output`; the BGRA pixel at `x, y` (output pixels).
bool GrabPixel(IDXGIOutput* output, ID3D11Device* dev, ID3D11DeviceContext* ctx, int x, int y,
               uint8_t bgra[4], HRESULT& hr) {
  ComPtr<IDXGIOutput1> out1;
  hr = output->QueryInterface(IID_PPV_ARGS(&out1));
  if (FAILED(hr)) return false;
  ComPtr<IDXGIOutputDuplication> dup;
  hr = out1->DuplicateOutput(dev, &dup);
  if (FAILED(hr)) return false;
  // The first frame after DuplicateOutput is the whole current desktop.
  for (int attempt = 0; attempt < 20; ++attempt) {
    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    hr = dup->AcquireNextFrame(250, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
    if (FAILED(hr)) return false;
    ComPtr<ID3D11Texture2D> frame;
    resource.As(&frame);
    D3D11_TEXTURE2D_DESC d{};
    frame->GetDesc(&d);
    if (info.LastPresentTime.QuadPart == 0 && attempt < 19) {  // pointer-only update
      dup->ReleaseFrame();
      continue;
    }
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    hr = dev->CreateTexture2D(&d, nullptr, &staging);
    if (SUCCEEDED(hr)) ctx->CopyResource(staging.Get(), frame.Get());
    dup->ReleaseFrame();
    if (FAILED(hr)) return false;
    D3D11_MAPPED_SUBRESOURCE m{};
    hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) return false;
    const bool inside = x >= 0 && y >= 0 && UINT(x) < d.Width && UINT(y) < d.Height;
    if (inside) memcpy(bgra, static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch + size_t(x) * 4, 4);
    ctx->Unmap(staging.Get(), 0);
    return inside;
  }
  return false;
}

// Puts a small magenta window on the primary screen, covers it with the
// screen-off curtain and checks what Desktop Duplication delivers: magenta
// (the curtain is hidden from capture), then black with exclusion turned off
// (the curtain really is on top). Needs an unlocked desktop; it shows a
// 320x200 rectangle near the top-left corner for about two seconds.
int CmdCurtainTest() {
  POINT origin{0, 0};
  HMONITOR hmon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(hmon, &mi)) return 1;
  auto f = CreateDxgiFactory();
  DxOutputInfo oi;
  if (!f || !FindOutput(f.Get(), mi.szDevice, oi)) {
    printf("primary output not found\n");
    return 1;
  }
  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  if (FAILED(CreateDeviceForAdapter(f.Get(), oi.adapterLuid, dev, ctx))) return 1;
  ComPtr<IDXGIOutput> output;
  for (UINT ai = 0; !output; ++ai) {
    ComPtr<IDXGIAdapter1> a;
    if (f->EnumAdapters1(ai, &a) == DXGI_ERROR_NOT_FOUND) break;
    for (UINT ox = 0;; ++ox) {
      ComPtr<IDXGIOutput> o;
      if (a->EnumOutputs(ox, &o) == DXGI_ERROR_NOT_FOUND) break;
      DXGI_OUTPUT_DESC od{};
      o->GetDesc(&od);
      if (_wcsicmp(od.DeviceName, mi.szDevice) == 0) {
        output = o;
        break;
      }
    }
  }
  if (!output) return 1;

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hbrBackground = CreateSolidBrush(RGB(255, 0, 255));
  wc.lpszClassName = L"WvdCurtainProbe";
  RegisterClassExW(&wc);
  RECT probe{mi.rcMonitor.left + 40, mi.rcMonitor.top + 40, mi.rcMonitor.left + 360,
             mi.rcMonitor.top + 240};
  HWND w = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName,
                           L"WVD curtain probe", WS_POPUP, probe.left, probe.top,
                           probe.right - probe.left, probe.bottom - probe.top, nullptr, nullptr,
                           wc.hInstance, nullptr);
  if (!w) return 1;
  ShowWindow(w, SW_SHOWNOACTIVATE);
  UpdateWindow(w);
  PumpFor(400);
  const int px = 200, py = 140;  // probe centre, in output pixels
  auto isMagenta = [](const uint8_t p[4]) { return p[2] > 200 && p[0] > 200 && p[1] < 60; };
  auto isBlack = [](const uint8_t p[4]) { return p[0] < 30 && p[1] < 30 && p[2] < 30; };
  auto show = [](const char* what, const uint8_t p[4]) {
    printf("  %-44s r=%3u g=%3u b=%3u\n", what, p[2], p[1], p[0]);
  };
  uint8_t base[4] = {}, hidden[4] = {}, shown[4] = {};
  HRESULT hr = S_OK;
  bool ok = GrabPixel(output.Get(), dev.Get(), ctx.Get(), px, py, base, hr);
  ScreenCurtain curtain;
  ScreenCurtain::Status excludedStatus{};
  if (ok) {
    show("test window, no curtain:", base);
    ScreenCurtain::Options o;
    o.useTestRect = true;
    o.testRect = probe;
    o.showHint = false;
    curtain.Start(o);
    excludedStatus = curtain.status();
    PumpFor(400);
    ok = GrabPixel(output.Get(), dev.Get(), ctx.Get(), px, py, hidden, hr);
    curtain.Stop();
    if (ok) show("curtain over it, excluded from capture:", hidden);
  }
  if (ok) {
    ScreenCurtain::Options o;
    o.useTestRect = true;
    o.testRect = probe;
    o.showHint = false;
    o.excludeFromCapture = false;
    curtain.Start(o);
    PumpFor(400);
    ok = GrabPixel(output.Get(), dev.Get(), ctx.Get(), px, py, shown, hr);
    curtain.Stop();
    if (ok) show("curtain over it, capture exclusion off:", shown);
  }
  DestroyWindow(w);
  PumpFor(50);
  for (const auto& m : EnumerateCurtainMonitors()) {
    auto name = WideToUtf8(m.device.c_str());
    printf("  %-14s %ldx%ld at (%ld,%ld)  %s\n", name.c_str(), m.rect.right - m.rect.left,
           m.rect.bottom - m.rect.top, m.rect.left, m.rect.top,
           m.virtualDisplay ? "virtual: never covered" : "covered by --screen-off");
  }
  if (!ok) {
    // Locked or secure desktop: still check that Windows accepts the curtain window.
    ScreenCurtain::Options o;
    o.useTestRect = true;
    o.testRect = probe;
    o.showHint = false;
    curtain.Start(o);
    const auto st = curtain.status();
    curtain.Stop();
    printf("curtain window: %s, excluded from capture: %s, Ctrl+Alt+Shift+B: %s\n",
           st.covered ? "created" : "NOT created", st.excluded ? "yes" : "NO",
           st.hotkey ? "registered" : "taken by another app");
    printf("capture failed hr=0x%08lx (unlocked interactive desktop required)\n", (unsigned long)hr);
    return 1;
  }
  const bool pass = isMagenta(base) && excludedStatus.covered == 1 && excludedStatus.excluded &&
                    isMagenta(hidden) && isBlack(shown);
  printf("%s: %s\n", pass ? "PASS" : "FAIL",
         !isMagenta(base)            ? "capture did not show the test window"
         : !excludedStatus.excluded  ? "Windows refused to exclude the curtain from capture"
         : !isMagenta(hidden)        ? "the curtain appears in the captured picture"
         : !isBlack(shown)           ? "the curtain did not cover the window"
                                     : "the curtain blacks out the screen and stays out of the stream");
  return pass ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  // Physical-pixel coordinates (§4.1). Runs before any display query.
  wvd::EnablePerMonitorDpiAwareness();

  log::Init("diag", log::Level::Info);
  if (argc < 2) {
    printf("wvd-diag: adapters|topology|caps|modes|capture|sink|owner-ping|coords-selftest|curtain-test\n");
    return 2;
  }
  std::string c = argv[1];
  if (c == "adapters") return CmdAdapters();
  if (c == "topology") return CmdTopology(argc, argv);
  if (c == "caps") return CmdCaps();
  if (c == "modes") return CmdModes();
  if (c == "capture") return CmdCapture(argc, argv);
  if (c == "sink") return CmdSink(argc, argv);
  if (c == "owner-ping") return CmdOwnerPing();
  if (c == "coords-selftest") return CmdCoordsSelftest();
  if (c == "curtain-test") return CmdCurtainTest();
  printf("unknown command\n");
  return 2;
}

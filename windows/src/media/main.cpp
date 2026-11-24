// "stream" role — media worker (§4.1 process roles).
//
// Owns: capture (Path D), preprocessing, encoder, transport, discovery.
// Does NOT own: the software device or topology (that is the "owner" role, the
// small stable "device owner"); if this process dies the monitors stay up.
//
// Usage:
//   ndisplay stream --demo 10              local pipeline check, prints stats
//   ndisplay stream --listen 47800         WIRE v1 over QUIC
// Options:
//   --monitor \\.\DISPLAY2 --fps 60 --bitrate 60000 --codec auto|null
//   --input \\.\pipe\wvd.input  forward input to privileged helper
#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include "discovery.h"
#include "session/media_session.h"
#include "workstation.h"
#include "wvd/logging.h"
#include "wvd/roles.h"
#include "wvd/version.h"
#include "wvd/stop_signal.h"

#include "identity.h"
#include "wdauth.h"
#include "wire1_channel.h"

using namespace wvd;

namespace {

std::atomic<bool> g_quit{false};

BOOL WINAPI CtrlHandler(DWORD type) {
  (void)type;
  g_quit = true;
  return TRUE;
}

// The panel's activity view only keeps the last minutes in memory, so a
// session that lagged or dropped could not be diagnosed after the fact. The
// listening host appends to stream.log in the identity store (WVM_HOME
// redirects it); past 16 MB the file moves to stream.prev.log at startup.
void OpenStreamLogFile() {
  const std::string home = WvmHomeDir();
  if (home.empty()) return;
  const std::wstring dir = Utf8ToWideStr(home);
  const std::wstring path = dir + L"\\stream.log";
  WIN32_FILE_ATTRIBUTE_DATA fa{};
  if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) &&
      ((ULONGLONG(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow) > (16ull << 20))
    MoveFileExW(path.c_str(), (dir + L"\\stream.prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);
  log::SetFile(path.c_str());
}


void Usage() {
  printf(
      "ndisplay stream — media worker\n"
      "usage: ndisplay stream [--demo N] [--listen PORT] [--monitor NAME]\n"
      "                 [--fps N] [--bitrate KBPS] [--codec auto|hevc|h264|ENCODER]\n"
      "                 [--input PIPE] [--input-token HEX] [--synthetic]\n"
      "                 [--max-pixels N] [--min-bitrate KBPS] [--fixed-bitrate]\n"
      "                 [--gop-sec N] [--box-chroma] [--no-mode-change]\n"
      "                 [--no-audio] [--screen-off] [--no-discovery]\n"
      "                 [--stereo-layout none|sbs|sbs-half|top-bottom|top-bottom-half]\n"
      "                 [--stereo-size WIDTHxHEIGHT[:half-sbs]]\n"
      "                 [--game-depth | --depth-test-pattern [--depth-test-drop N]] [-v]\n"
      "Listens on UDP 47800 by default (WIRE v1 over QUIC). --demo runs offline.\n"
      "  --quic [PORT] is also accepted. Pairing uses the local pipe shown in the log.\n"
      "  --pair-auto skips the code comparison and is intended for lab use.\n"
      "--no-pair stops new devices pairing (paired headsets still connect)\n"
      "--allow-input lets a paired headset drive mouse and keyboard; without\n"
      "  it a QUIC session streams picture and sound but cannot touch the PC\n"
      "--bitrate is the ceiling; the target drops while the link is behind and\n"
      "  recovers afterwards (--fixed-bitrate turns that off)\n"
      "--max-pixels caps the encoded picture (default 8294400, or 16588800 for\n"
      "  full side-by-side; 0 = no host pixel ceiling)\n"
      "--gop-sec N adds a periodic keyframe (default: on demand only)\n"
      "--no-mode-change stops the headset changing the monitor's size (by\n"
      "  default a size picked on the headset lasts until the session ends)\n"
      "--no-audio never sends this PC's sound, even when the headset asks\n"
      "--screen-off blacks out this PC's screens while a headset is connected\n"
      "  (Ctrl+Alt+Shift+B shows them again; the stream is not affected)\n"
      "--game-depth accepts scene depth from an opted-in D3D12 game with the\n"
      "  NDisplay ReShade add-on, while the headset requests 3D for that display\n"
      "--stereo-layout describes the existing packing on the shared display.\n"
      "  Full side-by-side falls back to half when the encoder or headset requires it.\n"
      "--stereo-size limits the packed frame size without upscaling it. Append\n"
      "  :half-sbs to squeeze a full SBS source and signal half SBS to the headset.\n"
      "--depth-test-pattern feeds the embedded-depth pipeline a synthetic map\n"
      "  (a ramp plus a square that follows the pointer); the headset still\n"
      "  has to ask for depth per monitor. --depth-test-drop N clears\n"
      "  DEPTH_VALID on every Nth frame to exercise the fallback path\n"
      "encoders: mf-hw, mf-sw-h264, nvenc-hevc (SDK build), null (test only)\n");
}

struct Args {
  int demoSeconds = 0;
  uint16_t listenPort = 0;
  std::wstring monitor;
  uint32_t fps = 60;
  uint32_t bitrate = 60000;
  std::string codec = "auto";
  std::wstring inputPipe = L"\\\\.\\pipe\\wvd.input";
  std::wstring stopEvent;
  bool boxChroma = false;
  bool noDiscovery = false;
  uint64_t inputToken = 0;
  bool synthetic = false;
  bool verbose = false;
  std::optional<uint64_t> maxPixels;
  msg::StereoLayout stereoLayout = msg::StereoLayout::None;
  StereoOutputSize stereoOutputSize;
  uint32_t minBitrate = 0;
  bool fixedBitrate = false;
  uint32_t gopSeconds = 0;
  uint32_t synWidth = 0, synHeight = 0;
  bool noModeChange = false;
  bool noAudio = false;
  bool screenOff = false;
  bool gameDepth = false;
  bool depthTestPattern = false;
  uint32_t depthTestDropEvery = 0;
  bool pairAuto = false;
  bool noPair = false;
  bool allowInput = false;
};

bool ParseStereoSize(std::string_view value, StereoOutputSize& size) {
  constexpr std::string_view halfSuffix = ":half-sbs";
  if (value.size() >= halfSuffix.size() &&
      value.substr(value.size() - halfSuffix.size()) == halfSuffix) {
    size.halfSbs = true;
    value.remove_suffix(halfSuffix.size());
  } else {
    size.halfSbs = false;
  }
  const size_t x = value.find('x');
  if (x == std::string_view::npos) return false;
  const auto parseDimension = [](std::string_view field, uint32_t& dimension) {
    const char* end = field.data() + field.size();
    const auto result = std::from_chars(field.data(), end, dimension);
    return result.ec == std::errc{} && result.ptr == end && dimension >= 4 &&
           dimension <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION && !(dimension & 1);
  };
  return parseDimension(value.substr(0, x), size.width) &&
         parseDimension(value.substr(x + 1), size.height);
}

bool ParseArgs(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    std::string s = argv[i];
    auto next = [&](std::string& out) {
      if (i + 1 >= argc) return false;
      out = argv[++i];
      return true;
    };
    std::string v;
    if (s == "--demo" && next(v)) {
      a.demoSeconds = atoi(v.c_str());
      if (a.demoSeconds <= 0) return false;
    } else if (s == "--listen" || s == "--quic") {
      if (s == "--quic" && (i + 1 == argc || argv[i + 1][0] == '-')) {
        a.listenPort = kQuicDefaultPort;
      } else {
        if (!next(v) || v.empty() || v.find_first_not_of("0123456789") != std::string::npos)
          return false;
        const auto port = strtoul(v.c_str(), nullptr, 10);
        if (port == 0 || port > 65535) return false;
        a.listenPort = static_cast<uint16_t>(port);
      }
    }
    else if (s == "--monitor" && next(v))
      a.monitor = Utf8ToWide(v.c_str());
    else if (s == "--fps" && next(v))
      a.fps = uint32_t(atoi(v.c_str()));
    else if (s == "--input-token" && next(v))
      a.inputToken = strtoull(v.c_str(), nullptr, 16);
    else if (s == "--bitrate" && next(v))
      a.bitrate = uint32_t(atoi(v.c_str()));
    else if (s == "--codec" && next(v))
      a.codec = v;
    else if (s == "--input" && next(v))
      a.inputPipe = Utf8ToWide(v.c_str());
    else if (s == "--stop-event" && next(v))
      a.stopEvent = Utf8ToWide(v.c_str());
    else if (s == "--max-pixels" && next(v))
      a.maxPixels = strtoull(v.c_str(), nullptr, 10);
    else if (s == "--stereo-layout" && next(v)) {
      using msg::StereoLayout;
      if (v == "none") a.stereoLayout = StereoLayout::None;
      else if (v == "sbs") a.stereoLayout = StereoLayout::SideBySide;
      else if (v == "sbs-half") a.stereoLayout = StereoLayout::SideBySideHalf;
      else if (v == "top-bottom") a.stereoLayout = StereoLayout::TopBottom;
      else if (v == "top-bottom-half") a.stereoLayout = StereoLayout::TopBottomHalf;
      else {
        fprintf(stderr, "unknown --stereo-layout: %s\n", v.c_str());
        return false;
      }
    }
    else if (s == "--stereo-size" && next(v)) {
      if (!ParseStereoSize(v, a.stereoOutputSize)) {
        fprintf(stderr, "invalid --stereo-size: %s\n", v.c_str());
        return false;
      }
    }
    else if (s == "--min-bitrate" && next(v))
      a.minBitrate = uint32_t(atoi(v.c_str()));
    else if (s == "--fixed-bitrate")
      a.fixedBitrate = true;
    else if (s == "--no-mode-change")
      a.noModeChange = true;
    else if (s == "--no-audio")
      a.noAudio = true;
    else if (s == "--screen-off")
      a.screenOff = true;
    else if (s == "--game-depth")
      a.gameDepth = true;
    else if (s == "--depth-test-pattern")
      a.depthTestPattern = true;
    else if (s == "--depth-test-drop" && next(v))
      a.depthTestDropEvery = uint32_t(atoi(v.c_str()));
    else if (s == "--pair-auto")
      a.pairAuto = true;
    else if (s == "--no-pair")
      a.noPair = true;
    else if (s == "--allow-input")
      a.allowInput = true;
    else if (s == "--gop-sec" && next(v))
      a.gopSeconds = uint32_t(atoi(v.c_str()));
    else if (s == "--synthetic-size" && next(v)) {
      if (sscanf(v.c_str(), "%ux%u", &a.synWidth, &a.synHeight) != 2) {
        Usage();
        return false;
      }
    } else if (s == "--box-chroma")
      a.boxChroma = true;
    else if (s == "--synthetic")
      a.synthetic = true;
    else if (s == "--no-discovery")
      a.noDiscovery = true;
    else if (s == "-v" || s == "--verbose")
      a.verbose = true;
    else {
      Usage();
      return false;
    }
  }
  if (a.gameDepth && (a.synthetic || a.depthTestPattern)) {
    fprintf(stderr, "--game-depth requires desktop capture and cannot be combined with --depth-test-pattern\n");
    return false;
  }
  if (a.stereoLayout != msg::StereoLayout::None && (a.gameDepth || a.depthTestPattern)) {
    fprintf(stderr, "--stereo-layout cannot be combined with game or synthetic depth\n");
    return false;
  }
  if ((a.stereoOutputSize.width && a.stereoLayout == msg::StereoLayout::None) ||
      (a.stereoOutputSize.halfSbs && a.stereoLayout != msg::StereoLayout::SideBySide)) {
    fprintf(stderr, "--stereo-size requires a stereo layout; :half-sbs requires --stereo-layout sbs\n");
    return false;
  }
  return true;
}

// The UI reads structured events on stdout and submits decisions on a
// process-specific local pipe. The QUIC worker never waits for the UI.
class PairingConsole : public IWire1Observer {
 public:
  explicit PairingConsole(Wire1Listener& listener) : listener_(listener) {}
  ~PairingConsole() {
    if (!thread_.joinable()) return;
    SetEvent(stop_);
    thread_.join();
    CloseHandle(pipe_);
    CloseHandle(stop_);
  }

  bool Start() {
    const auto name = L"\\\\.\\pipe\\wvm-pairing-" + std::to_wstring(GetCurrentProcessId());
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_) return false;
    pipe_ = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE |
                                 FILE_FLAG_OVERLAPPED,
                             PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
                                 PIPE_REJECT_REMOTE_CLIENTS,
                             1, 0, 256, 0, nullptr);
    if (pipe_ == INVALID_HANDLE_VALUE) {
      CloseHandle(stop_);
      return false;
    }
    thread_ = std::thread([this] { ReadLoop(); });
    WVD_LOGI("pairing console: %ls", name.c_str());
    return true;
  }

  void OnPairingRequest(uint64_t requestId, const std::string& deviceName,
                        const std::string& sas) override {
    auto name = deviceName.substr(0, 160);
    for (char& ch : name) if (static_cast<unsigned char>(ch) < 32) ch = ' ';
    printf("PAIR %llu %s %s\n", static_cast<unsigned long long>(requestId), sas.c_str(),
           name.c_str());
    fflush(stdout);
  }

  void OnPairingDecided(uint64_t requestId, bool success) override {
    printf("PAIRED %llu %s\n", static_cast<unsigned long long>(requestId),
           success ? "ok" : "rejected");
    fflush(stdout);
  }

 private:
  bool Complete(OVERLAPPED& io, DWORD& bytes) {
    const HANDLE events[] = {stop_, io.hEvent};
    if (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0) {
      CancelIoEx(pipe_, &io);
      GetOverlappedResult(pipe_, &io, &bytes, TRUE);
      return false;
    }
    return GetOverlappedResult(pipe_, &io, &bytes, FALSE) != FALSE;
  }

  void ReadLoop() {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) return;
    while (WaitForSingleObject(stop_, 0) == WAIT_TIMEOUT) {
      OVERLAPPED io{};
      io.hEvent = event;
      ResetEvent(event);
      DWORD bytes = 0;
      bool connected = ConnectNamedPipe(pipe_, &io) != FALSE;
      if (!connected) {
        const DWORD error = GetLastError();
        connected = error == ERROR_PIPE_CONNECTED ||
                    (error == ERROR_IO_PENDING && Complete(io, bytes));
      }
      if (connected) {
        char text[128] = {};
        io = {};
        io.hEvent = event;
        ResetEvent(event);
        bool read = ReadFile(pipe_, text, sizeof(text) - 1, &bytes, &io) != FALSE;
        if (!read && GetLastError() == ERROR_IO_PENDING) read = Complete(io, bytes);
        unsigned long long id = 0;
        char choice[16] = {};
        if (read && sscanf(text, "%llu %15s", &id, choice) == 2 &&
            (!strcmp(choice, "accept") || !strcmp(choice, "deny"))) {
          listener_.DecidePairing(id, strcmp(choice, "accept") == 0);
        }
      }
      DisconnectNamedPipe(pipe_);
    }
    CloseHandle(event);
  }

  Wire1Listener& listener_;
  HANDLE stop_ = nullptr;
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  std::thread thread_;
};

}  // namespace

namespace wvd {
int RoleStream(int argc, char** argv) {
  // Physical-pixel coordinates (§4.1). Runs before any display query.
  EnablePerMonitorDpiAwareness();

  Args a;
  if (!ParseArgs(argc, argv, a)) return 2;
  StopSignal stopSignal;
  if (!stopSignal.Watch(a.stopEvent, g_quit)) return 1;
  if (g_quit) return 0;
  log::Init("media", a.verbose ? log::Level::Verbose : log::Level::Info);
  if (!a.demoSeconds && !a.listenPort) a.listenPort = kQuicDefaultPort;
  if (a.listenPort) OpenStreamLogFile();
  // A captured log alone must say which host build produced it.
  WVD_LOGI("%s %s", kHostProduct, HostVersion().c_str());
  SetConsoleCtrlHandler(CtrlHandler, TRUE);
  RaiseProcessPriority();

  SessionConfig cfg;
  cfg.monitorName = a.monitor;
  cfg.fps = a.fps;
  cfg.bitrateKbps = a.bitrate;
  cfg.codec = a.codec;
  cfg.inputPipe = a.inputPipe;
  cfg.inputToken = a.inputToken;
  cfg.chroma = a.boxChroma ? ChromaFilter::Box2x2 : ChromaFilter::TextAware;
  cfg.maxEncodePixels = a.maxPixels;
  cfg.stereoLayout = a.stereoLayout;
  cfg.stereoOutputSize = a.stereoOutputSize;
  cfg.minBitrateKbps = a.minBitrate;
  cfg.adaptiveBitrate = !a.fixedBitrate;
  cfg.gopSeconds = a.gopSeconds;
  cfg.allowModeChange = !a.noModeChange;
  cfg.allowAudio = !a.noAudio;
  cfg.screenOff = a.screenOff;
  cfg.gameDepth = a.gameDepth;
  cfg.depthTestPattern = a.depthTestPattern;
  cfg.depthTestDropEvery = a.depthTestDropEvery;
  if (a.synWidth && a.synHeight) {
    cfg.synWidth = a.synWidth;
    cfg.synHeight = a.synHeight;
  }

  GenCounters gens;

  // Pin an explicit GDI name before accepting a headset. DISPLAYn can be
  // reassigned while the listener waits; the monitor interface follows the
  // selected monitor across reconnects without capturing an unrelated screen.
  if (!a.synthetic && !cfg.monitorName.empty()) {
    auto factory = CreateDxgiFactory();
    DxOutputInfo selected;
    if (!factory || !FindOutput(factory.Get(), cfg.monitorName.c_str(), selected)) {
      WVD_LOGE("selected display '%ls' is unavailable; refresh the display list and select an active display",
               cfg.monitorName.c_str());
      return 1;
    }
    if (!selected.monitorDevicePath.empty()) cfg.monitorName = selected.monitorDevicePath;
  }

  HostIdentity identity;
  PairedDeviceStore devices;
  std::unique_ptr<Wire1Listener> listener;
  std::unique_ptr<PairingConsole> console;
  DnsRegistration dns;
  WorkstationWatcher workstation;
  if (a.listenPort) {
    std::string error;
    if (!identity.Load("NDisplayHostWin", &error)) {
      WVD_LOGE("host identity: %s", error.c_str());
      return 1;
    }
    if (!devices.Load(&error))
      WVD_LOGW("paired devices: %s", error.c_str());

    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = L"Windows-PC";
    DWORD nameLength = ARRAYSIZE(name);
    GetComputerNameW(name, &nameLength);
    Wire1Config wireCfg;
    wireCfg.hostId = Wire1HostId(identity.certSha256());
    wireCfg.hostName = WideToUtf8(name);
    wireCfg.hostOs = "Windows";
    wireCfg.deadlineMs = cfg.deadlineMs;
    wireCfg.pairingAllowed = !a.noPair;
    wireCfg.autoAcceptPairing = a.pairAuto;
    wireCfg.inputAllowed = a.allowInput && a.inputToken != 0;
    listener = std::make_unique<Wire1Listener>(gens, wireCfg, identity, devices);
    if (wireCfg.pairingAllowed && !wireCfg.autoAcceptPairing) {
      console = std::make_unique<PairingConsole>(*listener);
      if (!console->Start()) {
        WVD_LOGE("could not open the local pairing pipe");
        return 1;
      }
      listener->SetObserver(console.get());
    }
    if (!listener->Listen(a.listenPort, &error)) {
      WVD_LOGE("listen on udp:%u failed: %s", a.listenPort, error.c_str());
      printf("HOST_LISTEN_ERROR UDP %u: %s\n", a.listenPort, error.c_str());
      fflush(stdout);
      return 1;
    }
    if (!a.noDiscovery) dns.Start(name, a.listenPort, wireCfg.hostId);
    if (!a.synthetic) workstation.Start();
    printf("HOST_READY %u\n", a.listenPort);
    fflush(stdout);
  }

  const ULONGLONG startTick = GetTickCount64();
  auto demoExpired = [&] {
    return a.demoSeconds &&
           GetTickCount64() - startTick >= ULONGLONG(a.demoSeconds) * 1000ull;
  };
  auto serviceHost = [&](Wire1Channel* channel) {
    dns.Refresh();
    if (!channel || a.synthetic) return WorkstationState::Available;
    const auto state = workstation.Poll();
    channel->SetWorkstationState(state);
    return state;
  };

  // Reconnect loop: the peer may drop and come back, and the plan requires a
  // reconnect to work without restarting the worker (§4.2). One session per
  // connection; generations keep advancing across them.
  for (;;) {
    if (demoExpired() || g_quit) break;

    std::shared_ptr<Wire1Channel> channel;
    if (listener) {
      while (!demoExpired() && !g_quit && !channel) {
        serviceHost(nullptr);
        channel = listener->AcceptChannel(200);
      }
      if (!channel) break;
      WVD_LOGI("headset connected over QUIC (%s), input %s", channel->peerName().c_str(),
               channel->inputAllowed() ? "armed" : "disarmed");
      while (!g_quit && !demoExpired() && channel->IsConnected() &&
             serviceHost(channel.get()) != WorkstationState::Available)
        Sleep(200);
      if (g_quit || demoExpired()) break;
      if (!channel->IsConnected()) continue;
    }

    MediaSession session(cfg, gens);
    session.synthetic = a.synthetic;
    HRESULT hr = session.Start(channel);
    if (FAILED(hr)) {
      WVD_LOGE("session start failed hr=%s", HresultStr(hr).c_str());
      if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
        WVD_LOGE("selected display is unavailable; refresh the display list and select an active display");
      else if (hr == E_ACCESSDENIED || hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
        WVD_LOGE("DDA needs an unlocked interactive desktop session; run from your logon");
      if (!listener) return 1;
      Sleep(1000);  // keep listening; the next attempt may succeed
      continue;
    }

    // Stats ticker + liveness.
    for (;;) {
      for (int i = 0; i < 5 && !g_quit && !session.peerGone(); ++i) {
        Sleep(200);
        serviceHost(channel.get());
      }
      auto s = session.GetSnapshot();
      WVD_LOGI(
          "cap=%llu enc=%llu sent=%llu late=%llu missed=%llu q[c/e/s]=%u/%u/%u "
          "conv p50=%.2fms p95=%.2fms | enc p50=%.2fms p95=%.2fms "
          "%ukbps %ufps ptr=%d enc=%s target=%ukbps size=%ux%u %s skipped=%llu "
          "depth=%s%ux%u/%llu gaps[cap/send]=%.2f/%.2fms send-p95=%.2fms",
          (unsigned long long)s.framesCaptured, (unsigned long long)s.framesEncoded,
          (unsigned long long)s.framesSent, (unsigned long long)s.framesDroppedLate,
          (unsigned long long)s.missedUpdates, s.capQueue, s.encQueue, s.sendQueue,
          s.convP50Ms, s.convP95Ms, s.encP50Ms, s.encP95Ms, s.kbpsOut, s.fpsOut,
          int(s.pointerState), s.encoder.c_str(), s.targetKbps, s.encWidth, s.encHeight,
          s.zeroCopy ? "gpu-texture" : "cpu-copy", (unsigned long long)s.skippedForLink,
          s.depthPacking ? "on" : "off", s.depthMapWidth, s.depthMapHeight,
          (unsigned long long)s.depthValidFrames,
          s.captureGapMaxMs, s.sendGapMaxMs, s.sendP95Ms);
      if (channel) {
        const auto link = channel->link();
        WVD_LOGI(
             "quic: datagrams-sent=%llu rtx-queued=%llu transport-drops=%llu pacer-dropped=%lluB loss=%.2f%% rtt=%.1fms FEC=%.0f%% "
            "pacer-backlog=%zuB",
            (unsigned long long)link.datagramsSent, (unsigned long long)link.retransmitted,
            (unsigned long long)link.datagramDrops, (unsigned long long)link.pacerDrops,
            link.lossPercent,
            link.rttMs, link.fecPercent, channel->BacklogBytes());
      }
      if (session.peerGone()) {
        WVD_LOGW("session ended (peer gone) — re-arming to accept again (§4.2)");
        break;
      }
      if (demoExpired() || g_quit) break;
    }
    session.Stop();
    if (!listener) break;  // demo mode: one pass is enough
    if (demoExpired() || g_quit) break;
    // fall through and accept the next connection
  }
  if (listener) listener->Stop();
  return 0;
}
}  // namespace wvd

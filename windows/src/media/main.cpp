// wvd-media — media worker (§4.1 process roles).
//
// Owns: capture (Path D), preprocessing, encoder, transport, discovery.
// Does NOT own: the software device or topology (that is wvd-owner.exe, the
// small stable "device owner"); if this process dies the monitors stay up.
//
// Usage:
//   wvd-media --demo 10                 local pipeline check, prints stats
//   wvd-media --listen 47800         WIRE v1 over QUIC
// Options:
//   --monitor \\.\DISPLAY2 --fps 60 --bitrate 60000 --codec auto|null
//   --input \\.\pipe\wvd.input  forward input to privileged helper
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "discovery.h"
#include "session/media_session.h"
#include "wvd/logging.h"
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


void Usage() {
  printf(
      "wvd-media — WVD media worker\n"
      "usage: wvd-media [--demo N] [--listen PORT] [--monitor NAME]\n"
      "                 [--fps N] [--bitrate KBPS] [--codec auto|hevc|h264|ENCODER]\n"
      "                 [--input PIPE] [--input-token HEX] [--synthetic]\n"
      "                 [--max-pixels N] [--min-bitrate KBPS] [--fixed-bitrate]\n"
      "                 [--gop-sec N] [--box-chroma] [--no-mode-change]\n"
      "                 [--no-audio] [--screen-off] [--no-discovery] [-v]\n"
      "Listens on UDP 47800 by default (WIRE v1 over QUIC). --demo runs offline.\n"
      "  --quic [PORT] is also accepted. Pairing uses the local pipe shown in the log.\n"
      "  --pair-auto skips the code comparison and is intended for lab use.\n"
      "--no-pair stops new devices pairing (paired headsets still connect)\n"
      "--allow-input lets a paired headset drive mouse and keyboard; without\n"
      "  it a QUIC session streams picture and sound but cannot touch the PC\n"
      "--bitrate is the ceiling; the target drops while the link is behind and\n"
      "  recovers afterwards (--fixed-bitrate turns that off)\n"
      "--max-pixels caps the encoded picture (default 8294400 = 3840x2160;\n"
      "  larger desktops are scaled to fit, 0 = encode at desktop size)\n"
      "--gop-sec N adds a periodic keyframe (default: on demand only)\n"
      "--no-mode-change stops the headset changing the monitor's size (by\n"
      "  default a size picked on the headset lasts until the session ends)\n"
      "--no-audio never sends this PC's sound, even when the headset asks\n"
      "--screen-off blacks out this PC's screens while a headset is connected\n"
      "  (Ctrl+Alt+Shift+B shows them again; the stream is not affected)\n"
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
  uint64_t maxPixels = 3840ull * 2160ull;
  uint32_t minBitrate = 0;
  bool fixedBitrate = false;
  uint32_t gopSeconds = 0;
  uint32_t synWidth = 0, synHeight = 0;
  bool noModeChange = false;
  bool noAudio = false;
  bool screenOff = false;
  bool pairAuto = false;
  bool noPair = false;
  bool allowInput = false;
};

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

int main(int argc, char** argv) {
  // Physical-pixel coordinates (§4.1). Runs before any display query.
  wvd::EnablePerMonitorDpiAwareness();

  Args a;
  if (!ParseArgs(argc, argv, a)) return 2;
  StopSignal stopSignal;
  if (!stopSignal.Watch(a.stopEvent, g_quit)) return 1;
  if (g_quit) return 0;
  log::Init("media", a.verbose ? log::Level::Verbose : log::Level::Info);
  SetConsoleCtrlHandler(CtrlHandler, TRUE);
  if (!a.demoSeconds && !a.listenPort) a.listenPort = kQuicDefaultPort;

  SessionConfig cfg;
  cfg.monitorName = a.monitor;
  cfg.fps = a.fps;
  cfg.bitrateKbps = a.bitrate;
  cfg.codec = a.codec;
  cfg.inputPipe = a.inputPipe;
  cfg.inputToken = a.inputToken;
  cfg.chroma = a.boxChroma ? ChromaFilter::Box2x2 : ChromaFilter::TextAware;
  cfg.maxEncodePixels = a.maxPixels;
  cfg.minBitrateKbps = a.minBitrate;
  cfg.adaptiveBitrate = !a.fixedBitrate;
  cfg.gopSeconds = a.gopSeconds;
  cfg.allowModeChange = !a.noModeChange;
  cfg.allowAudio = !a.noAudio;
  cfg.screenOff = a.screenOff;
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
      return 1;
    }
    if (!a.noDiscovery) dns.Start(name, a.listenPort, wireCfg.hostId);
    printf("HOST_READY %u\n", a.listenPort);
    fflush(stdout);
  }

  const ULONGLONG startTick = GetTickCount64();
  auto demoExpired = [&] {
    return a.demoSeconds &&
           GetTickCount64() - startTick >= ULONGLONG(a.demoSeconds) * 1000ull;
  };

  // Reconnect loop: the peer may drop and come back, and the plan requires a
  // reconnect to work without restarting the worker (§4.2). One session per
  // connection; generations keep advancing across them.
  for (;;) {
    if (demoExpired() || g_quit) break;

    std::shared_ptr<Wire1Channel> channel;
    if (listener) {
      while (!demoExpired() && !g_quit && !channel)
        channel = listener->AcceptChannel(200);
      if (!channel) break;
      WVD_LOGI("headset connected over QUIC (%s), input %s", channel->peerName().c_str(),
               channel->inputAllowed() ? "armed" : "disarmed");
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
      Sleep(1000);
      auto s = session.GetSnapshot();
      WVD_LOGI(
          "cap=%llu enc=%llu sent=%llu late=%llu missed=%llu q[c/e/s]=%u/%u/%u "
          "conv p50=%.2fms p95=%.2fms | enc p50=%.2fms p95=%.2fms "
          "%ukbps %ufps ptr=%d enc=%s target=%ukbps size=%ux%u %s skipped=%llu "
          "gaps[cap/send]=%.2f/%.2fms send-p95=%.2fms",
          (unsigned long long)s.framesCaptured, (unsigned long long)s.framesEncoded,
          (unsigned long long)s.framesSent, (unsigned long long)s.framesDroppedLate,
          (unsigned long long)s.missedUpdates, s.capQueue, s.encQueue, s.sendQueue,
          s.convP50Ms, s.convP95Ms, s.encP50Ms, s.encP95Ms, s.kbpsOut, s.fpsOut,
          int(s.pointerState), s.encoder.c_str(), s.targetKbps, s.encWidth, s.encHeight,
          s.zeroCopy ? "gpu-texture" : "cpu-copy", (unsigned long long)s.skippedForLink,
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

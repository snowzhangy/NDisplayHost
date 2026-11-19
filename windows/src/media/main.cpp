// wvd-media — media worker (§4.1 process roles).
//
// Owns: capture (Path D), preprocessing, encoder, transport, discovery.
// Does NOT own: the software device or topology (that is wvd-owner.exe, the
// small stable "device owner"); if this process dies the monitors stay up.
//
// Usage:
//   wvd-media --demo 10                 local pipeline check, prints stats
//   wvd-media --listen 50001            wait for the headset (TCP dev transport)
//   wvd-media --quic 47800              wait for the headset over QUIC (WIRE v1)
// Both listeners may run at once; each advertises its own service. Options:
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
#include "wvd/transport.h"

#ifdef WVD_HAVE_MSQUIC
#include "identity.h"
#include "wdauth.h"
#include "wire1_channel.h"
#endif

using namespace wvd;

namespace {

std::atomic<bool> g_quit{false};

// WIRE.md §2: the UDP port the headset connects to by default.
constexpr int kDefaultQuicPort = 47800;

BOOL WINAPI CtrlHandler(DWORD type) {
  (void)type;
  g_quit = true;
  return TRUE;
}


void Usage() {
  printf(
      "wvd-media — WVD media worker\n"
      "usage: wvd-media [--demo N] [--listen PORT] [--quic PORT] [--monitor NAME]\n"
      "                 [--fps N] [--bitrate KBPS] [--codec auto|hevc|h264|ENCODER]\n"
      "                 [--input PIPE] [--input-token HEX] [--synthetic]\n"
      "                 [--max-pixels N] [--min-bitrate KBPS] [--fixed-bitrate]\n"
      "                 [--gop-sec N] [--box-chroma] [--no-mode-change]\n"
      "                 [--no-audio] [--screen-off] [--no-discovery] [-v]\n"
      "--listen is the packed TCP host the current headset builds speak;\n"
      "  --quic is proto/WIRE.md v1 over UDP (default 47800) for Vision Pro\n"
      "  builds with the QUIC host. Pairing codes appear on stderr and on\n"
      "  \\\\.\\pipe\\wvm-pairing; --pair-auto accepts without a code check.\n"
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
  int listenPort = 0;
  int quicPort = 0;
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
    if (s == "--demo" && next(v))
      a.demoSeconds = atoi(v.c_str());
    else if (s == "--listen" && next(v))
      a.listenPort = atoi(v.c_str());
    else if (s == "--quic" && next(v))
      a.quicPort = atoi(v.c_str());
    else if (s == "--quic" && i + 1 == argc)
      a.quicPort = kDefaultQuicPort;
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

#ifdef WVD_HAVE_MSQUIC
// The pairing console (docs/PAIRING-CONSOLE.md). One line out per event, so a
// UI never has to parse the log:
//   PAIR <request-id> <6-digit code> <device name>   a headset wants to pair
//   PAIRED <request-id> ok|rejected                  how that request ended
// and in: "<request-id> accept\n" / "<request-id> deny\n".
class PairingConsole : public IWire1Observer {
 public:
  explicit PairingConsole(Wire1Listener& listener) : listener_(listener) {}
  ~PairingConsole() { Stop(); }

  bool Start() {
    pipe_ = CreateNamedPipeW(kPipe, PIPE_ACCESS_DUPLEX,
                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                 PIPE_REJECT_REMOTE_CLIENTS,
                             1, 4096, 4096, 0, nullptr);
    if (pipe_ == INVALID_HANDLE_VALUE) {
      WVD_LOGW("pairing console: pipe unavailable (err %lu) — use the log instead",
               GetLastError());
      return false;
    }
    running_ = true;
    thread_ = std::thread([this] { ReadLoop(); });
    WVD_LOGI("pairing console: %ls", kPipe);
    return true;
  }

  void Stop() {
    running_ = false;
    if (pipe_ != INVALID_HANDLE_VALUE) {
      const HANDLE h = pipe_;
      pipe_ = INVALID_HANDLE_VALUE;
      CancelSynchronousIo(h);
      DisconnectNamedPipe(h);
      CloseHandle(h);
    }
    if (thread_.joinable()) thread_.join();
  }

  void OnPairingRequest(uint64_t requestId, const std::string& deviceName,
                        const std::string& sas) override {
    WVD_LOGI("PAIRING: %s asks to pair — accept only if it shows the code %s",
             deviceName.c_str(), sas.c_str());
    Write("PAIR " + std::to_string(requestId) + " " + sas + " " + deviceName + "\n");
  }

  void OnPairingDecided(uint64_t requestId, bool success) override {
    Write("PAIRED " + std::to_string(requestId) + (success ? " ok\n" : " rejected\n"));
  }

 private:
  static constexpr const wchar_t* kPipe = L"\\\\.\\pipe\\wvm-pairing";

  void Write(const std::string& line) {
    std::lock_guard<std::mutex> lk(m_);
    if (pipe_ == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(pipe_, line.data(), DWORD(line.size()), &written, nullptr);
  }

  void ReadLoop() {
    std::string pending;
    while (running_) {
      const HANDLE h = pipe_;
      if (h == INVALID_HANDLE_VALUE) break;
      if (!ConnectNamedPipe(h, nullptr)) {
        const DWORD err = GetLastError();
        if (err != ERROR_PIPE_CONNECTED && err != ERROR_IO_PENDING && err != ERROR_OPERATION_ABORTED)
          Sleep(200);
        continue;
      }
      char buffer[256];
      DWORD got = 0;
      while (running_ && ReadFile(h, buffer, sizeof(buffer), &got, nullptr) && got) {
        pending.append(buffer, got);
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
          std::string line = pending.substr(0, nl);
          pending.erase(0, nl + 1);
          unsigned long long id = 0;
          char word[16] = {};
          if (sscanf(line.c_str(), "%llu %15s", &id, word) == 2)
            listener_.DecidePairing(id, strcmp(word, "accept") == 0);
        }
      }
    }
  }

  Wire1Listener& listener_;
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::mutex m_;
};
#endif  // WVD_HAVE_MSQUIC

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
  SetConsoleCtrlHandler(
      [](DWORD type) -> BOOL {
        if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT) {
          WVD_LOGI("ctrl-c — stopping");
          ExitProcess(0);  // threads do bounded waits; P0 keeps it simple
        }
        return FALSE;
      },
      TRUE);

  if (!a.demoSeconds && !a.listenPort && !a.quicPort) {
    WVD_LOGI("no mode given — defaulting to --demo 10");
    a.demoSeconds = 10;
  }

  WinsockGuard ws;
  if (!ws.ok()) {
    WVD_LOGE("winsock init failed");
    return 1;
  }

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

  TcpListener listener;
  DnsRegistration dns;
  if (a.listenPort) {
    if (!listener.Start(uint16_t(a.listenPort))) {
      WVD_LOGE("listen on %d failed", a.listenPort);
      return 1;
    }
    if (!a.noDiscovery) dns.Start(L"WVD-PC", uint16_t(a.listenPort));
    WVD_LOGI("waiting for headset on tcp:%d (packed dev transport)", a.listenPort);
  }

#ifdef WVD_HAVE_MSQUIC
  // The QUIC host (core/proto/WIRE.md) has pairing rooted in
  // the TLS exporter rather than in the network being friendly.
  HostIdentity identity;
  PairedDeviceStore devices;
  Wire1Config wireCfg;
  Wire1Listener* quicListener = nullptr;
  std::unique_ptr<Wire1Listener> quicListenerOwner;
  std::unique_ptr<PairingConsole> console;
  if (a.quicPort) {
    std::string error;
    if (!identity.Load("WVM Host", &error)) {
      WVD_LOGE("host identity: %s — the QUIC host stays off", error.c_str());
    } else {
      std::string storeError;
      if (!devices.Load(&storeError))
        WVD_LOGW("paired devices: %s — starting with an empty list", storeError.c_str());
      wireCfg.hostId = Wire1HostId(identity.certSha256());
      wireCfg.hostName = "Windows PC";
      wireCfg.pairingAllowed = !a.noPair;
      wireCfg.autoAcceptPairing = a.pairAuto;
      // A headset may watch the desktop without being allowed to drive it.
      wireCfg.inputAllowed = a.allowInput;
      WVD_LOGI("host id %s, %zu paired device(s)", wireCfg.hostId.c_str(), devices.all().size());
      auto owned = std::make_unique<Wire1Listener>(gens, wireCfg, identity, devices);
      if (owned->Listen(uint16_t(a.quicPort), &error)) {
        quicListener = owned.get();
        quicListenerOwner = std::move(owned);
        console = std::make_unique<PairingConsole>(*quicListener);
        console->Start();
        quicListener->SetObserver(console.get());
        WVD_LOGI("waiting for headset on udp:%d (WIRE v1 over QUIC, ALPN %s)", a.quicPort,
                 kQuicAlpn);
        if (!a.noDiscovery) dns.StartUdp(L"WVM-PC", uint16_t(a.quicPort), wireCfg.hostId);
      } else {
        WVD_LOGE("listen on udp:%d failed: %s", a.quicPort, error.c_str());
        return 1;
      }
    }
  }
  const bool quicListening = quicListener != nullptr;
#else
  const bool quicListening = false;
  if (a.quicPort)
    WVD_LOGE("--quic requires MsQuic; run windows/scripts/build-msquic.ps1 and rebuild");
#endif
  if (a.quicPort && !a.listenPort && !quicListening) return 1;

  SetConsoleCtrlHandler(CtrlHandler, TRUE);

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

    // A session's channel is shared: the QUIC transport keeps its own
    // reference alive while msquic callbacks still fire into it.
    std::shared_ptr<IReliableChannel> channel;
    if (a.listenPort || a.quicPort) {
      for (;;) {
#ifdef WVD_HAVE_MSQUIC
        if (quicListener) {
          auto quicChannel = quicListener->AcceptChannel(200);
          if (quicChannel) {
            channel = quicChannel;
            WVD_LOGI("headset connected over QUIC (%s), input %s",
                     quicChannel->peerName().c_str(),
                     quicChannel->inputAllowed() ? "armed" : "disarmed");
            break;
          }
        }
#endif
        if (a.listenPort) {
          channel = listener.Accept(200);
          if (channel) {
            WVD_LOGI("headset connected over TCP");
            break;
          }
        }
        if (demoExpired() || g_quit) break;
      }
      if (!channel) break;
    }

    MediaSession session(cfg, gens);
    session.synthetic = a.synthetic;
    HRESULT hr = session.Start(std::move(channel));
    if (FAILED(hr)) {
      WVD_LOGE("session start failed hr=%s", HresultStr(hr).c_str());
      if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
        WVD_LOGE("selected display is unavailable; refresh the display list and select an active display");
      else if (hr == E_ACCESSDENIED || hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
        WVD_LOGE("DDA needs an unlocked interactive desktop session; run from your logon");
      if (!a.listenPort && !a.quicPort) return 1;
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
#ifdef WVD_HAVE_MSQUIC
      if (quicListener && quicListener->active()) {
        const auto link = quicListener->active()->link();
        WVD_LOGI(
            "quic: shards=%llu rtx=%llu drops=%llu loss=%.2f%% rtt=%.1fms FEC=%.0f%% "
            "pacer-backlog=%zuB",
            (unsigned long long)link.shardsSent, (unsigned long long)link.retransmitted,
            (unsigned long long)(link.datagramDrops + link.pacerDrops), link.lossPercent,
            link.rttMs, link.fecPercent, quicListener->active()->pacerBacklog());
      }
#endif
      if (session.peerGone()) {
        WVD_LOGW("session ended (peer gone) — re-arming to accept again (§4.2)");
        break;
      }
      if (demoExpired() || g_quit) break;
    }
    session.Stop();
    if (!a.listenPort && !a.quicPort) break;  // demo mode: one pass is enough
    if (demoExpired() || g_quit) break;
    // fall through and accept the next connection
  }
  return 0;
}

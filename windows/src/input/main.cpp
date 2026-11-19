// Main owns notifications; only the hook-free injector thread calls SendInput.
#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include "input_state.h"
#include <wtsapi32.h>
#include "wvd/ipc.h"
#include "wvd/logging.h"
#include "wvd/stop_signal.h"
using namespace wvd;

namespace {
constexpr wchar_t kDefaultPipe[] = L"\\\\.\\pipe\\wvd.input";
struct QueuedEvent { uint64_t generation; InputEvent event; };
std::mutex g_stateMutex;
std::deque<QueuedEvent> g_queue;
InputGate g_gate;
InputInjector g_injector;
HANDLE g_wake = nullptr;
std::atomic<bool> g_stop{false};
uint64_t g_authToken = 0, g_ownerConnection = 0, g_instance = 0;
bool g_sessionActive = false, g_desktopReady = false, g_secureDesktop = false;
bool g_release = false, g_attach = true;
DWORD g_sessionId = 0;

// Caller holds g_stateMutex. Flush before any subsequent injection.
void Flush() { g_queue.clear(); g_release = true; SetEvent(g_wake); }
void Revoke() { Flush(); g_gate.Disarm(); g_ownerConnection = 0; }
void RefreshSuspension() { g_gate.Suspend(!g_sessionActive || !g_desktopReady); }
void DesktopChanged() {
  std::lock_guard<std::mutex> lock(g_stateMutex);
  Flush(); g_attach = true; g_desktopReady = false; RefreshSuspension();
}

bool SessionActive() {
  LPWSTR data = nullptr;
  DWORD bytes = 0;
  bool active = false;
  if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, g_sessionId,
                                  WTSSessionInfoEx, &data, &bytes)) {
    if (bytes >= sizeof(WTSINFOEXW)) {
      const auto* info = reinterpret_cast<const WTSINFOEXW*>(data);
      active = info->Level == 1 && info->Data.WTSInfoExLevel1.SessionState == WTSActive &&
               info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_UNLOCK;
    }
    WTSFreeMemory(data);
  }
  return active;  // fail closed if session state is unavailable
}

LRESULT CALLBACK NotificationProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
  if (message == WM_WTSSESSION_CHANGE && DWORD(lp) == g_sessionId) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    // Lock suspends consent; disconnect/logoff revokes it.
    if (wp == WTS_SESSION_LOCK || wp == WTS_CONSOLE_DISCONNECT ||
        wp == WTS_REMOTE_DISCONNECT || wp == WTS_SESSION_LOGOFF) {
      g_sessionActive = false;
      Flush();
      if (wp != WTS_SESSION_LOCK) Revoke();
    } else if (wp == WTS_SESSION_UNLOCK || wp == WTS_CONSOLE_CONNECT ||
               wp == WTS_REMOTE_CONNECT || wp == WTS_SESSION_LOGON) {
      g_sessionActive = SessionActive(); g_attach = true; g_desktopReady = false;
    }
    RefreshSuspension(); SetEvent(g_wake);
    return 0;
  }
  return DefWindowProcW(window, message, wp, lp);
}

std::optional<std::vector<uint8_t>> OnRequest(const uint8_t* data, size_t len) {
  auto packet = PacketView::Parse(data, len);
  if (!packet || packet->kind() != PacketKind::IpcCommand) return std::nullopt;
  auto hdr = packet->Get<IpcHeader>();
  if (!hdr) return std::nullopt;
  const auto connection = PipeServer::CurrentConnectionId();
  std::lock_guard<std::mutex> lock(g_stateMutex);
  uint32_t detail = 0;
  if (hdr->cmd != CmdPing && hdr->callerAuth != g_authToken) detail |= 0x4;
  else {
    switch (hdr->cmd) {
      case CmdPing: break;
      case InputArm:
        if (g_ownerConnection != connection) {
          Revoke(); g_ownerConnection = connection; g_gate.Arm();
        }
        break;
      case InputDisarm: Revoke(); break;
      case InputReleaseAll:
        if (g_ownerConnection == connection) Flush();
        else detail |= 0x2;
        break;
      case InputInject: {
        // Command data follows IpcHeader.
        auto input = packet->Get<InputPayload>(sizeof(IpcHeader));
        if (!input || input->count > 256) { detail |= 0x2; break; }
        auto events = packet->ArrayAt<InputEvent>(sizeof(IpcHeader) + sizeof(InputPayload),
                                                  input->count);
        if (events.size() != input->count || g_ownerConnection != connection ||
            !g_gate.Accept(input->inputGen)) { detail |= 0x2; break; }
        if (g_queue.size() + events.size() > 1024) {
          // Never drop just a key-up while retaining its key-down.
          Revoke(); detail |= 0x2; break;
        }
        for (const auto& event : events) g_queue.push_back({input->inputGen, event});
        SetEvent(g_wake);
        break;
      }
      case InputGetState: break;
      default: return std::nullopt;
    }
  }
  IpcStatusResponse status{};
  status.state = g_gate.state() == InputGate::State::Armed ? 1u : 0u;
  status.detail = detail | (g_secureDesktop ? 1u : 0u) | (g_gate.suspended() ? 0x8u : 0u);
  status.inputGen = g_gate.generation();  // post-command state, including InputArm
  status.layoutGen = g_instance;  // distinguish helper restart from explicit revocation
  status.sessionGen = g_injector.heldCount();  // separate from status bits
  PacketWriter response(PacketKind::IpcResponse);
  response.Append(status);
  return response.Take();
}
void Disconnected() {
  std::lock_guard<std::mutex> lock(g_stateMutex);
  // Closing an unrelated ping connection must not revoke the worker.
  if (g_ownerConnection == PipeServer::CurrentConnectionId()) Revoke();
}

void InjectorThread() {
  DesktopTracker desktop;
  ULONGLONG nextProbe = 0;
  std::wstring previousName;
  while (!g_stop) {
    {
      std::lock_guard<std::mutex> lock(g_stateMutex);
      if (g_release) { g_injector.ReleaseAllHeld(); g_release = g_injector.heldCount() != 0; }
      const auto now = GetTickCount64();
      // Poll too: notifications may be coalesced or missed at startup.
      if (g_attach || now >= nextProbe) {
        std::wstring name;
        bool secure = false;
        if (!desktop.AttachCurrentThreadToInputDesktop(name, secure)) {
          Flush(); g_desktopReady = false;
        } else {
          if (previousName != name) Flush();
          previousName = name; g_secureDesktop = secure; g_desktopReady = true;
        }
        g_attach = false; nextProbe = now + 100; RefreshSuspension();
      }
      if (g_release) { g_injector.ReleaseAllHeld(); g_release = g_injector.heldCount() != 0; }
      if (!g_release && !g_queue.empty()) {
        const auto item = g_queue.front(); g_queue.pop_front();
        if (g_gate.Accept(item.generation)) g_injector.InjectEvent(item.event);
        continue;
      }
    }
    WaitForSingleObject(g_wake, 30);
  }
  g_injector.ReleaseAllHeld();
}
BOOL WINAPI StopHandler(DWORD type) {
  if (type != CTRL_C_EVENT && type != CTRL_CLOSE_EVENT && type != CTRL_BREAK_EVENT &&
      type != CTRL_SHUTDOWN_EVENT && type != CTRL_LOGOFF_EVENT) return FALSE;
  g_stop = true; SetEvent(g_wake); return TRUE;
}
}  // namespace

int main(int argc, char** argv) {
  EnablePerMonitorDpiAwareness(); log::Init("input");
  std::wstring pipe = kDefaultPipe;
  std::wstring stopEvent;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--pipe" && i + 1 < argc) pipe = Utf8ToWide(argv[++i]);
    else if (arg == "--stop-event" && i + 1 < argc) stopEvent = Utf8ToWide(argv[++i]);
    else if (arg == "--token" && i + 1 < argc) g_authToken = strtoull(argv[++i], nullptr, 16);
    else if (arg == "-v" || arg == "--verbose") log::SetLevel(log::Level::Verbose);
    else return 2;
  }
  StopSignal stopSignal;
  if (!stopSignal.Watch(stopEvent, g_stop)) return 1;
  if (g_stop) return 0;
  {
    using RandomFn = BOOLEAN(WINAPI*)(PVOID, ULONG);
    auto random = reinterpret_cast<RandomFn>(GetProcAddress(GetModuleHandleW(L"advapi32.dll"),
                                                           "SystemFunction036"));
    if (!random || !random(&g_instance, sizeof(g_instance)) || !g_instance) return 1;
    if (!g_authToken && (!random(&g_authToken, sizeof(g_authToken)) || !g_authToken)) return 1;
  }
  printf("WVD input arming token: %016llX\n", (unsigned long long)g_authToken);
  printf("start the media worker with: wvd-media --input-token %016llX\n",
         (unsigned long long)g_authToken);
  fflush(stdout);
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &g_sessionId) || !g_sessionId) {
    WVD_LOGE("launch wvd-input in the interactive session (not Session 0)"); return 1;
  }
  g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!g_wake) return 1;
  g_sessionActive = SessionActive();
  WNDCLASSW wc{};
  wc.lpfnWndProc = NotificationProc; wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"WvdInputNotifications";
  RegisterClassW(&wc);
  HWND window = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                nullptr, nullptr, wc.hInstance, nullptr);
  DesktopTracker watcher;
  if (!window || !WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION) ||
      !watcher.StartWatchingOnCurrentThread(DesktopChanged)) {
    WVD_LOGE("input notifications unavailable; refusing to inject");
    if (window) DestroyWindow(window);
    CloseHandle(g_wake); return 1;
  }
  PipeServer server;
  server.SetDisconnectHandler(Disconnected);
  if (!server.Start(pipe.c_str(), PipeServer::DefaultSddl().c_str(), OnRequest, 4)) {
    WTSUnRegisterSessionNotification(window); DestroyWindow(window);
    CloseHandle(g_wake); return 1;
  }
  SetConsoleCtrlHandler(StopHandler, TRUE);
  std::thread injector(InjectorThread);
  WVD_LOGI("input ready; injection DISARMED until consent");
  while (!g_stop) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message); DispatchMessageW(&message);
    }
    MsgWaitForMultipleObjects(0, nullptr, FALSE, 30, QS_ALLINPUT);
  }
  server.Stop(); SetEvent(g_wake); injector.join();
  WTSUnRegisterSessionNotification(window); DestroyWindow(window);
  SetConsoleCtrlHandler(StopHandler, FALSE); CloseHandle(g_wake);
  return 0;
}

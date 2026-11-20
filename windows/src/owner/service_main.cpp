// "owner" role — device owner service (§4.1) + host service duties (§4.2).
//
//   owns: SwDevice handle, monitor topology (CCD save/restore/watchdog),
//         power keep-awake on request, local control pipe
//   never: parses network input, encodes, or sends media
//
// usage:
//   ndisplay owner --install | --uninstall | --start | --stop
//   ndisplay owner --run-console        (interactive debugging)
//   ndisplay owner --arrange <nameMatch> arrange WVD monitor right of primary
//   ndisplay owner --save-topology | --restore-topology | --list
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>

#include "device.h"
#include "wvd/power.h"
#include "topology.h"
#include "wvd/file.h"
#include "wvd/ipc.h"
#include "wvd/logging.h"
#include "wvd/protocol.h"
#include "wvd/roles.h"
#include "wvd/version.h"

using namespace wvd;

namespace {

constexpr wchar_t kServiceName[] = L"WvdOwner";
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\wvd.owner";
constexpr wchar_t kTopoFile[] = L"topology.bin";

std::atomic<bool> g_stop{false};
SERVICE_STATUS_HANDLE g_ssh = nullptr;
DeviceOwner g_device;
KeepAwake g_awake;
std::atomic<uint32_t> g_state{0};  // 0 idle, 1 session, 2 error
bool g_asService = false;
std::mutex g_topologyMutex;
std::mutex g_controlMutex;

// CCD acts on the calling PROCESS's session. Impersonating a user in Session 0
// is insufficient. The LocalSystem service launches only this fixed one-shot
// command in the console session; no client-controlled executable/arguments.
bool RunConsoleTopology(bool recoverOnly, std::wstring& err) {
  std::lock_guard<std::mutex> lock(g_topologyMutex);
  if (!g_asService) return CcdRestore(DataPath(kTopoFile), err);
  const DWORD session = WTSGetActiveConsoleSessionId();
  if (session == 0xFFFFFFFF || !session || g_stop) { err = L"no active console session"; return false; }
  HANDLE processToken = nullptr, childToken = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &processToken)) return false;
  BOOL ok = DuplicateTokenEx(processToken, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation,
                             TokenPrimary, &childToken);
  CloseHandle(processToken);
  if (!ok) return false;
  DWORD targetSession = session;
  ok = SetTokenInformation(childToken, TokenSessionId, &targetSession, sizeof(targetSession));
  wchar_t exe[32768]{};
  const DWORD length = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
  std::wstring command = L"\"" + std::wstring(exe) + L"\" " +
                         (recoverOnly ? L"--recover-topology" : L"--restore-topology");
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.lpDesktop = const_cast<wchar_t*>(L"winsta0\\default");
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION process{};
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  ok = ok && length && length < ARRAYSIZE(exe) && job &&
       SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) &&
       CreateProcessAsUserW(childToken, exe, command.data(), nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process);
  const DWORD launchError = GetLastError();
  CloseHandle(childToken);
  if (!ok) {
    if (job) CloseHandle(job);
    err = L"console topology helper launch failed " + std::to_wstring(launchError); return false;
  }
  if (!AssignProcessToJobObject(job, process.hProcess)) {
    TerminateProcess(process.hProcess, ERROR_PROCESS_ABORTED);
    CloseHandle(process.hThread); CloseHandle(process.hProcess); CloseHandle(job);
    err = L"cannot contain console topology helper"; return false;
  }
  ResumeThread(process.hThread);
  CloseHandle(process.hThread);
  const auto deadline = GetTickCount64() + 30000;
  while (!g_stop && GetTickCount64() < deadline &&
         WaitForSingleObject(process.hProcess, 100) == WAIT_TIMEOUT) {}
  DWORD exitCode = STILL_ACTIVE;
  GetExitCodeProcess(process.hProcess, &exitCode);
  CloseHandle(job);  // kills an unfinished child before device teardown
  WaitForSingleObject(process.hProcess, INFINITE);
  CloseHandle(process.hProcess);
  if (exitCode != 0) err = L"console topology helper failed " + std::to_wstring(exitCode);
  return exitCode == 0;
}

bool EnableServicePrivilege(const wchar_t* name) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
  TOKEN_PRIVILEGES privileges{};
  privileges.PrivilegeCount = 1;
  bool ok = LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid) != FALSE;
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  SetLastError(ERROR_SUCCESS);
  ok = ok && AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) &&
       GetLastError() == ERROR_SUCCESS;
  CloseHandle(token); return ok;
}

std::optional<std::vector<uint8_t>> OnPipeRequest(const uint8_t* data, size_t len) {
  auto pv = PacketView::Parse(data, len);
  if (!pv || pv->kind() != PacketKind::IpcCommand) return std::nullopt;
  auto hdr = pv->Get<IpcHeader>();
  if (!hdr) return std::nullopt;
  std::lock_guard<std::mutex> control(g_controlMutex);

  IpcStatusResponse st{};
  st.state = g_state.load();
  st.sessionGen = 0;
  st.layoutGen = 0;

  switch (hdr->cmd) {
    case CmdPing:
      break;
    case OwnerSessionStart:
      g_state = 1;
      g_awake.Start(L"WVD session", true);
      WVD_LOGI("session start requested");
      break;
    case OwnerSessionStop:
      g_state = 0;
      g_awake.Stop();
      // Optional LockWorkStation() on end — deliberate, tray-driven (§4.2).
      WVD_LOGI("session stop requested");
      break;
    case OwnerRestoreTopology: {
      std::wstring err;
      st.detail = RunConsoleTopology(false, err) ? 1u : 0u;
      if (!st.detail) WVD_LOGW("restore: %ls", err.c_str());
      break;
    }
    case OwnerSetKeepAwake:
      if (hdr->flags & 1)
        g_awake.Start(L"WVD stay-connected", true);
      else
        g_awake.Stop();
      break;
    default:
      return std::nullopt;
  }

  PacketWriter w(PacketKind::IpcResponse);
  w.Append(st);
  return w.Take();
}

void WatchdogLoop() {
  // §4.2 crash-safe restore: if a saved topology exists and the layout
  // drifted (driver host restart, console-session change §10), restore.
  int backoffSec = 5;
  while (!g_stop) {
    for (int i = 0; i < backoffSec * 10 && !g_stop; ++i) Sleep(100);
    if (g_stop) break;
    std::wstring file = DataPath(kTopoFile);
    std::vector<uint8_t> blob;
    if (!ReadFileWhole(file, blob)) continue;
    std::wstring err;
    if (g_asService) {
      if (RunConsoleTopology(true, err)) backoffSec = 5;
      else {
        WVD_LOGW("topology watchdog: %ls", err.c_str());
        backoffSec = (std::min)(backoffSec * 2, 300);
      }
      continue;
    }
    // Only restore when an expected monitor actually disappeared (driver host
    // restart, session change). The user rearranging monitors in Settings is
    // intent, not drift — restoring on any rect change fought the user.
    if (!CcdSnapshotLost(file, err)) {
      backoffSec = 5;
      continue;
    }
    WVD_LOGW("watchdog: expected monitor lost (%ls) — restoring", err.c_str());
    if (CcdRestore(file, err))
      backoffSec = 5;
    else
      backoffSec = (std::min)(backoffSec * 2, 300);
  }
}

int RunLoop(bool asService, SERVICE_STATUS_HANDLE ssh) {
  g_asService = asService;
  if (asService && (!EnableServicePrivilege(SE_TCB_NAME) ||
                    !EnableServicePrivilege(SE_ASSIGNPRIMARYTOKEN_NAME) ||
                    !EnableServicePrivilege(SE_INCREASE_QUOTA_NAME))) {
    WVD_LOGE("service lacks privileges to launch console topology helper");
  }
  std::wstring err;
  if (!g_device.Start(err)) WVD_LOGW("device not created yet: %ls", err.c_str());

  PipeServer pipe;
  if (!pipe.Start(kPipeName, PipeServer::DefaultSddl().c_str(), OnPipeRequest, 4))
    WVD_LOGE("control pipe failed — remote control unavailable");

  std::thread wd(WatchdogLoop);

  if (asService) {
    SERVICE_STATUS ss{};
    ss.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    ss.dwCurrentState = SERVICE_RUNNING;
    ss.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    ss.dwCheckPoint = 0;
    ss.dwWaitHint = 3000;
    SetServiceStatus(ssh, &ss);
    while (!g_stop) Sleep(250);
  } else {
    SetConsoleCtrlHandler(
      [](DWORD t) -> BOOL {
        if (t == CTRL_C_EVENT || t == CTRL_CLOSE_EVENT) { g_stop = true; return TRUE; }
        return FALSE;
      }, TRUE);
    WVD_LOGI("owner role in console mode — Ctrl+C to stop");
    while (!g_stop) Sleep(250);
  }

  g_stop = true;
  if (wd.joinable()) wd.join();
  pipe.Stop();
  g_awake.Stop();
  g_device.Stop();
  if (asService) {
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(ssh, &status);  // only after callbacks/threads/device are gone
  }
  return 0;
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
  g_ssh = RegisterServiceCtrlHandlerW(kServiceName, [](DWORD op) {
    SERVICE_STATUS ss{};
    ss.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    // Must be re-stated on EVERY status report: leaving it zero told the SCM we
    // accept no controls after the first query, so the service could not be
    // stopped.
    ss.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    if (op == SERVICE_CONTROL_STOP || op == SERVICE_CONTROL_SHUTDOWN) {
      ss.dwCurrentState = SERVICE_STOP_PENDING;
      ss.dwControlsAccepted = 0;
      ss.dwWaitHint = 35000;
      g_stop = true;
    } else if (g_stop) {
      ss.dwCurrentState = SERVICE_STOP_PENDING;
      ss.dwControlsAccepted = 0;
      ss.dwWaitHint = 35000;
    } else {
      ss.dwCurrentState = SERVICE_RUNNING;
    }
    SetServiceStatus(g_ssh, &ss);
  });
  RunLoop(true, g_ssh);
}

}  // namespace

namespace wvd {
int RoleOwner(int argc, char** argv) {
  // Physical-pixel coordinates (§4.1). Runs before any display query.
  EnablePerMonitorDpiAwareness();

  log::Init("owner");
  WVD_LOGI("%s %s", kHostProduct, HostVersion().c_str());

  if (argc >= 2) {
    std::string a = argv[1];
    if (a == "--install" || a == "--uninstall" || a == "--start" || a == "--stop") {
      SC_HANDLE m = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
      if (!m) {
        WVD_LOGE("OpenSCManager failed gle=%lu (run elevated)", GetLastError());
        return 1;
      }
      if (a == "--install") {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring command = L"\"" + std::wstring(exe) + L"\"";
        SC_HANDLE s = CreateServiceW(m, kServiceName, L"WVD Device Owner",
                                     SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                     SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, command.c_str(),
                                     nullptr, nullptr, nullptr, nullptr, nullptr);
        if (!s) WVD_LOGE("CreateService gle=%lu", GetLastError());
        else {
          WVD_LOGI("service installed (LocalSystem — holds SwDevice + input helpers)");
          CloseServiceHandle(s);
        }
        CloseServiceHandle(m);
        return 0;
      }
      SC_HANDLE s = OpenServiceW(m, kServiceName, SERVICE_ALL_ACCESS);
      if (!s) {
        WVD_LOGE("OpenService gle=%lu", GetLastError());
        CloseServiceHandle(m);
        return 1;
      }
      bool ok = false;
      if (a == "--uninstall") ok = DeleteService(s) != FALSE;
      else if (a == "--start") ok = StartServiceW(s, 0, nullptr) != FALSE;
      else {
        SERVICE_STATUS st{};
        ok = ControlService(s, SERVICE_CONTROL_STOP, &st) != FALSE;
      }
      if (!ok) WVD_LOGE("%s failed gle=%lu", a.c_str(), GetLastError());
      CloseServiceHandle(s);
      CloseServiceHandle(m);
      return ok ? 0 : 1;
    }
    if (a == "--list") {
      std::vector<CcdMonitor> mons;
      std::wstring err;
      if (!CcdEnumerate(mons, err)) {
        wprintf(L"CCD failed: %ls\n", err.c_str());
        return 1;
      }
      for (auto& m : mons)
        wprintf(L"path=%u target=%u %-40ls %ls%ls %ld,%ld %ux%u\n", m.pathId,
                m.targetId, m.friendly.c_str(), m.activePath ? L"active " : L"idle  ",
                m.attachedToTarget ? L"attached" : L"detached", m.x, m.y, m.width,
                m.height);
      return 0;
    }
    if (a == "--save-topology") {
      std::wstring err;
      return CcdSave(DataPath(kTopoFile), err) ? 0 : (wprintf(L"%ls\n", err.c_str()), 1);
    }
    if (a == "--restore-topology") {
      std::wstring err;
      return CcdRestore(DataPath(kTopoFile), err) ? 0 : (wprintf(L"%ls\n", err.c_str()), 1);
    }
    if (a == "--recover-topology") {
      std::wstring err;
      const auto file = DataPath(kTopoFile);
      if (!CcdSnapshotLost(file, err)) return err.empty() ? 0 : 1;
      return CcdRestore(file, err) ? 0 : 1;
    }
    if (a == "--arrange" && argc >= 3) {
      std::wstring err;
      std::wstring match = Utf8ToWide(argv[2]);
      return CcdArrangeRight(match, err) ? 0 : (wprintf(L"%ls\n", err.c_str()), 1);
    }
    if (a == "--run-console") return RunLoop(false, nullptr);
    // fallthrough: service dispatch below
  }

  // Service dispatch.
  g_ssh = nullptr;
  SERVICE_TABLE_ENTRYW table[] = {{LPWSTR(kServiceName), (LPSERVICE_MAIN_FUNCTIONW)ServiceMain},
                                  {nullptr, nullptr}};
  // When launched by SCM this enters ServiceMain; from a console it fails
  // and we run interactively.
  if (!StartServiceCtrlDispatcherW(table)) {
    if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
      return RunLoop(false, nullptr);
    WVD_LOGE("StartServiceCtrlDispatcher gle=%lu", GetLastError());
    return 1;
  }
  return 0;
}
}  // namespace wvd

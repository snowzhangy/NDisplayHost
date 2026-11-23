#include "workstation.h"

#include <wtsapi32.h>

namespace wvd {

LRESULT CALLBACK WorkstationWatcher::WindowProc(HWND window, UINT message, WPARAM wparam,
                                               LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  } else if (message == WM_WTSSESSION_CHANGE) {
    auto* watcher = reinterpret_cast<WorkstationWatcher*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (watcher && wparam == WTS_SESSION_LOCK) watcher->locked_ = true;
    if (watcher && wparam == WTS_SESSION_UNLOCK) watcher->locked_ = false;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

void WorkstationWatcher::Start() {
  WNDCLASSW windowClass{};
  windowClass.lpfnWndProc = WindowProc;
  windowClass.hInstance = GetModuleHandleW(nullptr);
  windowClass.lpszClassName = L"NDisplayWorkstationWatcher";
  if (RegisterClassW(&windowClass)) {
    window_ = CreateWindowExW(0, windowClass.lpszClassName, L"", 0, 0, 0, 0, 0,
                              HWND_MESSAGE, nullptr, windowClass.hInstance, this);
    if (window_) WTSRegisterSessionNotification(window_, NOTIFY_FOR_THIS_SESSION);
  }
}

WorkstationWatcher::~WorkstationWatcher() {
  if (!window_) return;
  WTSUnRegisterSessionNotification(window_);
  DestroyWindow(window_);
}

WorkstationState WorkstationWatcher::Poll() {
  MSG message{};
  while (window_ && PeekMessageW(&message, window_, WM_WTSSESSION_CHANGE,
                                WM_WTSSESSION_CHANGE, PM_REMOVE)) {
    DispatchMessageW(&message);
  }

  // State can change before registration and the Winlogon/UAC secure desktop
  // can take over without a WTS lock notification.
  LPWSTR data = nullptr;
  DWORD bytes = 0;
  if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION,
                                  WTSSessionInfoEx, &data, &bytes)) {
    if (bytes >= sizeof(WTSINFOEXW)) {
      const auto* info = reinterpret_cast<const WTSINFOEXW*>(data);
      if (info->Level == 1) {
        const auto flags = info->Data.WTSInfoExLevel1.SessionFlags;
        if (flags == WTS_SESSIONSTATE_LOCK) locked_ = true;
        if (flags == WTS_SESSIONSTATE_UNLOCK) locked_ = false;
      }
    }
    WTSFreeMemory(data);
  }
  if (locked_) return WorkstationState::Locked;
  wchar_t desktopName[128]{};
  HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
  if (!desktop) return WorkstationState::SecureDesktop;
  const bool readable = GetUserObjectInformationW(desktop, UOI_NAME, desktopName,
                                                  sizeof(desktopName), nullptr) != FALSE;
  CloseDesktop(desktop);
  return readable && _wcsicmp(desktopName, L"Default") == 0
             ? WorkstationState::Available : WorkstationState::SecureDesktop;
}

}  // namespace wvd

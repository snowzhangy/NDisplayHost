#include "screen_curtain.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <utility>

#include "wvd/logging.h"

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011  // Windows 10 2004+
#endif

namespace wvd {
namespace {

constexpr wchar_t kClass[] = L"WvdScreenCurtain";
constexpr int kHotkeyId = 0x5756;
constexpr UINT kRebuildMsg = WM_APP + 1;
constexpr UINT kHintMs = 10000;

bool ContainsNoCase(const wchar_t* hay, const wchar_t* needle) {
  const size_t n = wcslen(needle);
  for (; *hay; ++hay) {
    size_t i = 0;
    while (i < n && hay[i] && towupper(hay[i]) == towupper(needle[i])) ++i;
    if (i == n) return true;
  }
  return false;
}

BOOL CALLBACK AddMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM out) {
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  if (GetMonitorInfoW(monitor, &mi)) {
    CurtainMonitor m;
    m.device = mi.szDevice;
    m.rect = mi.rcMonitor;
    reinterpret_cast<std::vector<CurtainMonitor>*>(out)->push_back(std::move(m));
  }
  return TRUE;
}

}  // namespace

bool IsVirtualMonitorPath(const wchar_t* path, DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY tech) {
  if (tech == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED ||
      tech == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL)
    return true;
  // The WVD driver's EDID manufacturer is "WVD", so its PnP id is DISPLAY\WVDxxxx
  // (it reports an embedded-DisplayPort connector, not an indirect one).
  return path && ContainsNoCase(path, L"#WVD");
}

std::vector<CurtainMonitor> EnumerateCurtainMonitors() {
  // GDI source name -> virtual (every target of that source is virtual).
  std::vector<std::pair<std::wstring, bool>> virtualBySource;
  for (int attempt = 0; attempt < 3; ++attempt) {
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) break;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    const LONG r = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr);
    if (r == ERROR_INSUFFICIENT_BUFFER) continue;  // topology changed in between
    if (r != ERROR_SUCCESS) break;
    paths.resize(np);
    for (const auto& p : paths) {
      DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
      src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
      src.header.size = sizeof(src);
      src.header.adapterId = p.sourceInfo.adapterId;
      src.header.id = p.sourceInfo.id;
      if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
      DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
      target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
      target.header.size = sizeof(target);
      target.header.adapterId = p.targetInfo.adapterId;
      target.header.id = p.targetInfo.id;
      const bool named = DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS;
      const bool isVirtual = IsVirtualMonitorPath(named ? target.monitorDevicePath : nullptr,
                                                  p.targetInfo.outputTechnology);
      auto it = std::find_if(virtualBySource.begin(), virtualBySource.end(), [&](const auto& e) {
        return _wcsicmp(e.first.c_str(), src.viewGdiDeviceName) == 0;
      });
      if (it == virtualBySource.end()) virtualBySource.emplace_back(src.viewGdiDeviceName, isVirtual);
      else it->second = it->second && isVirtual;  // a clone that includes a real screen is covered
    }
    break;
  }
  std::vector<CurtainMonitor> out;
  EnumDisplayMonitors(nullptr, nullptr, AddMonitor, reinterpret_cast<LPARAM>(&out));
  for (auto& m : out) {
    for (const auto& e : virtualBySource)
      if (_wcsicmp(e.first.c_str(), m.device.c_str()) == 0) m.virtualDisplay = e.second;
  }
  return out;
}

bool ScreenCurtain::Start(const Options& options) {
  Stop();
  HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!ready) return false;
  th_ = std::thread([this, options, ready] {
    threadId_ = GetCurrentThreadId();
    Run(options, ready);
  });
  WaitForSingleObject(ready, INFINITE);  // Run always signals, after its windows are up
  CloseHandle(ready);
  return true;
}

void ScreenCurtain::Stop() {
  if (!th_.joinable()) return;
  if (const DWORD id = threadId_.load()) PostThreadMessageW(id, WM_QUIT, 0, 0);
  th_.join();
  threadId_ = 0;
}

ScreenCurtain::Status ScreenCurtain::status() const {
  std::lock_guard<std::mutex> lk(statusMutex_);
  return status_;
}

void ScreenCurtain::Run(Options options, HANDLE ready) {
  // The message queue exists before Start returns, so Stop's WM_QUIT is never lost.
  MSG msg;
  PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // physical pixels
  options_ = std::move(options);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);  // fails harmlessly when a previous session registered it

  const bool hotkey =
      RegisterHotKey(nullptr, kHotkeyId, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'B') != 0;
  {
    std::lock_guard<std::mutex> lk(statusMutex_);
    status_.hotkey = hotkey;
  }
  hidden_ = false;
  hint_ = options_.showHint;
  Rebuild();
  SetEvent(ready);

  UINT_PTR hintTimer = hint_ ? SetTimer(nullptr, 0, kHintMs, nullptr) : 0;
  // Other topmost windows (a tool set "always on top") can rise above the
  // curtain; put it back on top every second.
  const UINT_PTR topTimer = SetTimer(nullptr, 0, 1000, nullptr);
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (msg.hwnd) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      continue;
    }
    if (msg.message == WM_HOTKEY && msg.wParam == kHotkeyId) {
      hidden_ = !hidden_;
      for (HWND w : windows_) ShowWindow(w, hidden_ ? SW_HIDE : SW_SHOWNOACTIVATE);
      WVD_LOGI("screen off: %s (Ctrl+Alt+Shift+B)", hidden_ ? "PC screens shown" : "PC screens off again");
    } else if (msg.message == WM_TIMER && hintTimer && msg.wParam == hintTimer) {
      KillTimer(nullptr, hintTimer);
      hintTimer = 0;
      hint_ = false;
      for (HWND w : windows_) InvalidateRect(w, nullptr, TRUE);
    } else if (msg.message == WM_TIMER && msg.wParam == topTimer) {
      if (!hidden_)
        for (HWND w : windows_)
          SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    } else if (msg.message == kRebuildMsg) {
      // Every curtain window reports a display change: rebuild once.
      while (PeekMessageW(&msg, nullptr, kRebuildMsg, kRebuildMsg, PM_REMOVE)) {
      }
      Rebuild();
    }
  }
  if (hintTimer) KillTimer(nullptr, hintTimer);
  KillTimer(nullptr, topTimer);
  DestroyWindows();
  if (font_) DeleteObject(font_);
  font_ = nullptr;
  fontDpi_ = 0;
  if (hotkey) UnregisterHotKey(nullptr, kHotkeyId);
}

void ScreenCurtain::DestroyWindows() {
  for (HWND w : windows_) DestroyWindow(w);
  windows_.clear();
}

void ScreenCurtain::Rebuild() {
  DestroyWindows();
  struct Target {
    RECT rect;
    bool streamed;
  };
  std::vector<Target> targets;
  if (options_.useTestRect) {
    targets.push_back({options_.testRect, true});
  } else {
    for (const auto& m : EnumerateCurtainMonitors()) {
      if (m.virtualDisplay) continue;  // no one looks at it
      targets.push_back({m.rect, _wcsicmp(m.device.c_str(), options_.capturedDevice.c_str()) == 0});
    }
  }
  Status st;
  st.excluded = true;
  for (const auto& t : targets) {
    HWND w = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        kClass, L"WVD screen off", WS_POPUP, t.rect.left, t.rect.top, t.rect.right - t.rect.left,
        t.rect.bottom - t.rect.top, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!w) {
      WVD_LOGW("screen off: cannot create a curtain window (gle=%lu)", GetLastError());
      continue;
    }
    SetLayeredWindowAttributes(w, 0, 255, LWA_ALPHA);  // opaque, and click-through with WS_EX_TRANSPARENT
    const bool excluded =
        options_.excludeFromCapture && SetWindowDisplayAffinity(w, WDA_EXCLUDEFROMCAPTURE) != 0;
    if (!excluded && t.streamed && options_.excludeFromCapture) {
      // It would stream black: leave the streamed screen visible instead.
      DestroyWindow(w);
      ++st.skipped;
      st.excluded = false;
      continue;
    }
    if (!excluded) st.excluded = false;
    windows_.push_back(w);
  }
  for (HWND w : windows_) {
    if (!hidden_) ShowWindow(w, SW_SHOWNOACTIVATE);
    SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
  st.covered = static_cast<int>(windows_.size());
  std::lock_guard<std::mutex> lk(statusMutex_);
  st.hotkey = status_.hotkey;
  status_ = st;
}

LRESULT CALLBACK ScreenCurtain::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  auto* self = reinterpret_cast<ScreenCurtain*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_NCCREATE:
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
      break;
    case WM_NCHITTEST:
      return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_DISPLAYCHANGE:
      // Monitors were added, removed or resized (for example by the headset's size menu).
      PostThreadMessageW(GetCurrentThreadId(), kRebuildMsg, 0, 0);
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      RECT rc;
      GetClientRect(hwnd, &rc);
      FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
      if (self && self->hint_) {
        const int dpi = static_cast<int>(GetDpiForWindow(hwnd));
        if (!self->font_ || self->fontDpi_ != dpi) {
          if (self->font_) DeleteObject(self->font_);
          self->font_ = CreateFontW(-MulDiv(14, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
          self->fontDpi_ = dpi;
        }
        HGDIOBJ old = SelectObject(dc, self->font_);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(96, 96, 96));
        const wchar_t* text =
            L"This PC's screen is off while it streams to Apple Vision Pro.\n"
            L"Press Ctrl+Alt+Shift+B to show it.";
        const UINT format = DT_CENTER | DT_NOPREFIX | DT_WORDBREAK;
        RECT measure = rc;  // DT_VCENTER takes one line only: centre the block by hand
        DrawTextW(dc, text, -1, &measure, format | DT_CALCRECT);
        RECT place = rc;
        place.top = rc.top + ((rc.bottom - rc.top) - (measure.bottom - measure.top)) / 2;
        place.bottom = place.top + (measure.bottom - measure.top);
        DrawTextW(dc, text, -1, &place, format);
        SelectObject(dc, old);
      }
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;  // WM_PAINT fills the whole window
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace wvd

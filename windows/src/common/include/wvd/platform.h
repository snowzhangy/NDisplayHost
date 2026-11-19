// WVD common: platform includes and small utilities.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2def.h>
#include <windows.h>
#include <winerror.h>

#include <cstdint>
#include <cstdarg>
#include <string>

namespace wvd {

// Small helpers used across all processes.

// Per-monitor-v2 DPI awareness. The embedded wvd.manifest is authoritative; this
// runtime call is a safety net for cases where the manifest did not apply, and
// must run before any display/coordinate API is touched.
//
// Why it matters (§4.1 Path D): without DPI awareness,
// DXGI_OUTPUT_DESC.DesktopCoordinates is returned in DPI-virtualised units
// (e.g. 1280x800 on a 2560x1600 display at 200%), while Desktop Duplication
// still hands over a 2560x1600 texture. The full-frame copy between different
// sizes then fails silently and nothing is ever sent.
inline void EnablePerMonitorDpiAwareness() {
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (!user32) user32 = LoadLibraryExW(L"user32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!user32) return;
  using Fn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
  auto fn = reinterpret_cast<Fn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
  if (fn && !fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
    // Already set (e.g. by the manifest) — that is fine.
  }
}

// DPI of a monitor (shcore), with a 96 fallback. Returns (dpiX, dpiY).
inline POINT MonitorDpi(const RECT& desktopCoords) {
  POINT dpi{96, 96};
  UINT dx = 96, dy = 96;
  HMODULE shcore = GetModuleHandleW(L"shcore.dll");
  if (!shcore) shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!shcore) return dpi;
  using Fn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
  auto fn = reinterpret_cast<Fn>(GetProcAddress(shcore, "GetDpiForMonitor"));
  if (!fn) return dpi;
  // The monitor nearest the given desktop rect.
  RECT r = desktopCoords;
  HMONITOR hm = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
  if (!hm)
    hm = MonitorFromPoint(POINT{desktopCoords.left, desktopCoords.top}, MONITOR_DEFAULTTONEAREST);
  if (hm && SUCCEEDED(fn(hm, 0 /*MDT_EFFECTIVE_DPI*/, &dx, &dy))) {
    dpi.x = LONG(dx);
    dpi.y = LONG(dy);
  }
  return dpi;
}

inline std::string WideToUtf8(const wchar_t* w) {
  if (!w) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string s(static_cast<size_t>(n) - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

inline std::wstring Utf8ToWide(const char* u8) {
  if (!u8) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, u8, -1, nullptr, 0);
  if (n <= 0) return {};
  std::wstring s(static_cast<size_t>(n) - 1, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, u8, -1, s.data(), n);
  return s;
}

inline std::wstring Utf8ToWideStr(const std::string& s) { return Utf8ToWide(s.c_str()); }

inline std::string HresultStr(HRESULT hr) {
  char buf[96];
  _snprintf_s(buf, sizeof(buf), _TRUNCATE, "0x%08lX (%lu)",
              static_cast<unsigned long>(hr),
              static_cast<unsigned long>(hr & 0xFFFFFF));
  return buf;
}

inline std::wstring LastWin32ErrorStr(const char* what) {
  DWORD e = GetLastError();
  LPWSTR msg = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, e, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
  std::wstring s = Utf8ToWide(what) + L": " + (msg ? msg : L"?") + L" (GLE=" + std::to_wstring(e) + L")";
  if (msg) LocalFree(msg);
  return s;
}

}  // namespace wvd

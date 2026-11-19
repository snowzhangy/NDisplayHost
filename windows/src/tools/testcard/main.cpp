// wvd-testcard — §8 measurement helper: big per-frame binary counter +
// moving marker so the camera rig can count *distinct* displayed frames and
// cross-check "unexpected missed updates". Press any key to flip a full
// screen patch (content-latency trigger), ESC to quit.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "wvd/pacing.h"
#include "wvd/platform.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {

constexpr wchar_t kClass[] = L"WvdTestcardWnd";
UINT64 g_frame = 0;
bool g_patch = false;
bool g_quit = false;

HBRUSH GetSolidBrush(COLORREF c) { return CreateSolidBrush(c); }

void DrawCounterBitcols(HDC dc, RECT rc, UINT64 value) {
  // 64-bit counter as bit columns (unambiguous at distance / through lens).
  int n = 0;
  for (int b = 63; b >= 0; --b)
    if (value & (1ull << b)) { n = 63 - b + 1; break; }
  if (n < 1) n = 1;
  int w = (rc.right - rc.left) * 3 / 4;
  int x0 = rc.left + (rc.right - rc.left) / 8;
  int cw = w / 64;
  int top = rc.top + (rc.bottom - rc.top) / 3;
  int bh = (rc.bottom - rc.top) / 3;
  for (int b = 0; b < 64; ++b) {
    RECT cell{ x0 + b * cw, top, x0 + (b + 1) * cw, top + bh };
    bool on = (value >> b) & 1;
    HBRUSH br = GetSolidBrush(RGB(on ? 255 : 0, on ? 255 : 0, on ? 255 : 0));
    FillRect(dc, &cell, br);
    DeleteObject(br);
  }
  (void)n;
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_KEYDOWN:
      if (w == VK_ESCAPE) g_quit = true;
      g_patch = !g_patch;
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc;
      GetClientRect(h, &rc);
      HBRUSH bg = CreateSolidBrush(g_patch ? RGB(10, 10, 10) : RGB(240, 240, 240));
      FillRect(dc, &rc, bg);
      DeleteObject(bg);
      DrawCounterBitcols(dc, rc, g_frame);
      // Moving marker: exact position encodes frame parity + vsync slot.
      int t = int(g_frame % 60);
      RECT dot{ rc.left + t * (rc.right - rc.left) / 60, rc.bottom - 80,
                rc.left + t * (rc.right - rc.left) / 60 + 40, rc.bottom - 40 };
      HBRUSH d = CreateSolidBrush(RGB(220, 30, 30));
      FillRect(dc, &dot, d);
      DeleteObject(d);
      wchar_t buf[64];
      swprintf(buf, 64, L"frame %llu", (unsigned long long)g_frame);
      SetTextColor(dc, g_patch ? RGB(255, 255, 255) : RGB(0, 0, 0));
      SetBkMode(dc, TRANSPARENT);
      TextOutW(dc, rc.left + 20, rc.top + 20, buf, int(wcslen(buf)));
      EndPaint(h, &ps);
      return 0;
    }
    case WM_DESTROY:
      g_quit = true;
      return 0;
  }
  return DefWindowProc(h, m, w, l);
}

}  // namespace

int main(int argc, char** argv) {
  // Physical-pixel coordinates (§4.1). Runs before any display query.
  wvd::EnablePerMonitorDpiAwareness();

  int w = 640, hgt = 360;
  double fps = 60.0;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--big")) { w = 1280; hgt = 720; }
    else if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atof(argv[++i]);
  }
  if (fps < 1.0) fps = 60.0;

  WNDCLASSW wc{};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = kClass;
  RegisterClassW(&wc);

  HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, kClass, L"WVD TESTCARD",
                              WS_POPUP | WS_VISIBLE, 80, 80, w, hgt, nullptr,
                              nullptr, wc.hInstance, nullptr);
  if (!hwnd) return 1;
  ShowWindow(hwnd, SW_SHOW);

  // Redraw at exactly `fps` (default 60; --fps 120 to exercise the host's
  // pacing). Sleep(16) is not usable for this: it rounds up to the system
  // timer tick and yields ~32 or ~64 fps depending on what else is running.
  wvd::PrecisionSleeper sleeper;
  const int64_t interval = wvd::MsToQpc(1000.0 / fps);
  int64_t due = wvd::QpcNow();
  while (!g_quit) {
    ++g_frame;
    InvalidateRect(hwnd, nullptr, FALSE);
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    due += interval;
    if (due < wvd::QpcNow()) due = wvd::QpcNow();
    sleeper.SleepUntilQpc(due);
  }
  return 0;
}

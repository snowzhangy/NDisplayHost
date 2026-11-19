// "PC screen off while streaming" (SessionConfig::screenOff, --screen-off): the
// Windows counterpart of the Mac's own display going dark while Mac Virtual
// Display runs.
//
// Every physical monitor gets a black, topmost, click-through window that is
// excluded from capture (WDA_EXCLUDEFROMCAPTURE, Windows 10 2004+). The
// monitors stay on and in the topology, so Desktop Duplication — and therefore
// the headset — still sees the desktop beneath the curtain, windows do not
// move, and input reaches whatever is under the pointer. Nothing is persisted:
// if the process exits for any reason, its windows and the curtain go with it.
// When capture exclusion is unavailable the streamed monitor is left
// uncovered, so the headset never receives a black picture.
//
// Ctrl+Alt+Shift+B shows the screens again (and blanks them again) for the
// rest of the session. Not covered: the secure desktop (UAC, lock screen),
// shell flyouts that sit above topmost windows, and the hardware pointer. An
// LCD's backlight stays on; an OLED panel shows true black.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wvd {

struct CurtainMonitor {
  std::wstring device;  // \\.\DISPLAYn
  RECT rect{};          // desktop coordinates, physical pixels
  bool virtualDisplay = false;
};

// Active monitors with their GDI names and rectangles. A monitor is virtual
// when it is a WVD virtual display or an indirect (IddCx-style) display that
// no one looks at; those are never covered.
std::vector<CurtainMonitor> EnumerateCurtainMonitors();
bool IsVirtualMonitorPath(const wchar_t* monitorDevicePath,
                          DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY outputTechnology);

class ScreenCurtain {
 public:
  struct Options {
    std::wstring capturedDevice;  // monitor being streamed ("" = none)
    bool showHint = true;         // dim instructions for the first seconds
    bool excludeFromCapture = true;
    // Test hook (wvd-diag curtain-test): cover only this rectangle.
    bool useTestRect = false;
    RECT testRect{};
  };
  struct Status {
    int covered = 0;          // monitors (or the test rectangle) blacked out
    int skipped = 0;          // streamed monitors left visible: no capture exclusion
    bool excluded = false;    // every curtain window is excluded from capture
    bool hotkey = false;      // Ctrl+Alt+Shift+B registered
  };

  ScreenCurtain() = default;
  ~ScreenCurtain() { Stop(); }
  ScreenCurtain(const ScreenCurtain&) = delete;
  ScreenCurtain& operator=(const ScreenCurtain&) = delete;

  // Starts the curtain thread and waits until its windows are up.
  bool Start(const Options& options);
  void Stop();
  bool Running() const { return th_.joinable(); }
  Status status() const;

 private:
  void Run(Options options, HANDLE ready);
  void Rebuild();
  void DestroyWindows();
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

  std::thread th_;
  std::atomic<DWORD> threadId_{0};
  // Curtain thread only:
  Options options_;
  std::vector<HWND> windows_;
  bool hidden_ = false;
  bool hint_ = false;
  HFONT font_ = nullptr;
  int fontDpi_ = 0;
  mutable std::mutex statusMutex_;
  Status status_;
};

}  // namespace wvd

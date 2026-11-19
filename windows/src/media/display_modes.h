// Display modes of the streamed monitor, for the headset's size menu.
//
// Mac Virtual Display offers a few aspect presets on the headset and, when one
// is picked, asks the Mac to re-apply the virtual display's modes. Here the
// monitor is whatever Windows drives (the WVD IddCx monitor, or a physical
// one in development), so the menu lists the sizes that monitor reports and a
// pick becomes a session-scoped mode change. The capture, encoder and layout
// then follow through the normal resize path (DDA ACCESS_LOST).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wvd/platform.h"

namespace wvd {

struct DisplayModeInfo {
  uint32_t width = 0, height = 0;
  uint32_t refreshHz = 0;
  bool current = false;  // the monitor runs this size right now
  bool native = false;   // the monitor's preferred size
};

// One entry per size, largest first. Per size the refresh rate is the
// monitor's current one when that size offers it, otherwise the highest it
// offers. Sizes under 1280x720 are left out; the current and native sizes are
// always listed. A rotated monitor lists only its current mode (resizing a
// rotated output is not supported). Empty if the device is unknown.
std::vector<DisplayModeInfo> ListDisplayModes(const std::wstring& gdiDeviceName,
                                              size_t maxCount = 32);

// The monitor's current mode. False if the device is unknown.
bool CurrentDisplayMode(const std::wstring& gdiDeviceName, DisplayModeInfo& out);

// Switches the monitor to width x height at refreshHz (0 = the rate the list
// would pick). Dynamic: not written to the registry, so it lasts until
// RestoreDisplayMode, the next mode change, or sign-out. Returns DISP_CHANGE_*.
LONG ApplyDisplayMode(const std::wstring& gdiDeviceName, uint32_t width, uint32_t height,
                      uint32_t refreshHz);

// Back to the mode stored for this monitor (undoes ApplyDisplayMode).
LONG RestoreDisplayMode(const std::wstring& gdiDeviceName);

}  // namespace wvd

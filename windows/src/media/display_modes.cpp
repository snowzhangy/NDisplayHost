#include "display_modes.h"

#include <algorithm>
#include <map>
#include <utility>

namespace wvd {

namespace {

bool Usable(const DEVMODEW& dm) {
  return dm.dmBitsPerPel == 32 && !(dm.dmDisplayFlags & DM_INTERLACED) && dm.dmPelsWidth &&
         dm.dmPelsHeight && dm.dmDisplayFrequency > 1;  // 0/1 = "hardware default"
}

// The target's preferred (native) size, via the CCD path whose source is
// this GDI device.
bool PreferredSize(const std::wstring& gdi, uint32_t& w, uint32_t& h) {
  UINT32 pathCount = 0, modeCount = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
    return false;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                         modes.data(), nullptr) != ERROR_SUCCESS)
    return false;
  for (UINT32 i = 0; i < pathCount; ++i) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = paths[i].sourceInfo.adapterId;
    source.header.id = paths[i].sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
        _wcsicmp(source.viewGdiDeviceName, gdi.c_str()) != 0)
      continue;
    DISPLAYCONFIG_TARGET_PREFERRED_MODE preferred{};
    preferred.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_PREFERRED_MODE;
    preferred.header.size = sizeof(preferred);
    preferred.header.adapterId = paths[i].targetInfo.adapterId;
    preferred.header.id = paths[i].targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&preferred.header) != ERROR_SUCCESS) return false;
    w = preferred.width;
    h = preferred.height;
    return w && h;
  }
  return false;
}

// Every usable mode, grouped by size: size -> refresh rates offered.
std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> SizesAndRates(
    const std::wstring& gdi) {
  std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> sizes;
  for (DWORD i = 0;; ++i) {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsExW(gdi.c_str(), i, &dm, 0)) break;
    if (!Usable(dm)) continue;
    auto& rates = sizes[{dm.dmPelsWidth, dm.dmPelsHeight}];
    if (std::find(rates.begin(), rates.end(), dm.dmDisplayFrequency) == rates.end())
      rates.push_back(dm.dmDisplayFrequency);
  }
  return sizes;
}

uint32_t PickRate(const std::vector<uint32_t>& rates, uint32_t currentHz) {
  if (std::find(rates.begin(), rates.end(), currentHz) != rates.end()) return currentHz;
  return rates.empty() ? 0 : *std::max_element(rates.begin(), rates.end());
}

}  // namespace

bool CurrentDisplayMode(const std::wstring& gdiDeviceName, DisplayModeInfo& out) {
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  if (gdiDeviceName.empty() ||
      !EnumDisplaySettingsExW(gdiDeviceName.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0))
    return false;
  out = {};
  out.width = dm.dmPelsWidth;
  out.height = dm.dmPelsHeight;
  out.refreshHz = dm.dmDisplayFrequency;
  out.current = true;
  return out.width && out.height;
}

std::vector<DisplayModeInfo> ListDisplayModes(const std::wstring& gdiDeviceName,
                                              size_t maxCount) {
  std::vector<DisplayModeInfo> list;
  DisplayModeInfo current;
  if (!CurrentDisplayMode(gdiDeviceName, current)) return list;
  DEVMODEW cur{};
  cur.dmSize = sizeof(cur);
  EnumDisplaySettingsExW(gdiDeviceName.c_str(), ENUM_CURRENT_SETTINGS, &cur, 0);
  uint32_t nativeW = 0, nativeH = 0;
  const bool haveNative = PreferredSize(gdiDeviceName, nativeW, nativeH);
  current.native = haveNative && nativeW == current.width && nativeH == current.height;
  if ((cur.dmFields & DM_DISPLAYORIENTATION) && cur.dmDisplayOrientation != DMDO_DEFAULT) {
    list.push_back(current);
    return list;
  }

  for (const auto& [size, rates] : SizesAndRates(gdiDeviceName)) {
    DisplayModeInfo m;
    m.width = size.first;
    m.height = size.second;
    m.native = haveNative && m.width == nativeW && m.height == nativeH;
    m.current = m.width == current.width && m.height == current.height;
    m.refreshHz = m.current ? current.refreshHz : PickRate(rates, current.refreshHz);
    if (!m.current && !m.native && (m.width < 1280 || m.height < 720)) continue;
    if (!m.current && m.width < m.height) continue;  // landscape desktop, landscape sizes
    list.push_back(m);
  }
  if (std::none_of(list.begin(), list.end(), [](const DisplayModeInfo& m) { return m.current; }))
    list.push_back(current);  // the driver did not enumerate its own current mode

  std::sort(list.begin(), list.end(), [](const DisplayModeInfo& a, const DisplayModeInfo& b) {
    const uint64_t pa = uint64_t(a.width) * a.height, pb = uint64_t(b.width) * b.height;
    return pa != pb ? pa > pb : a.width > b.width;
  });
  if (list.size() > maxCount) {
    // Keep the current and native sizes when trimming the long tail.
    std::vector<DisplayModeInfo> kept;
    for (const auto& m : list)
      if (kept.size() + 2 < maxCount || m.current || m.native) kept.push_back(m);
    list = std::move(kept);
    if (list.size() > maxCount) list.resize(maxCount);
  }
  return list;
}

LONG ApplyDisplayMode(const std::wstring& gdiDeviceName, uint32_t width, uint32_t height,
                      uint32_t refreshHz) {
  DisplayModeInfo current;
  if (!CurrentDisplayMode(gdiDeviceName, current)) return DISP_CHANGE_BADPARAM;
  if (!refreshHz) {
    const auto sizes = SizesAndRates(gdiDeviceName);
    const auto it = sizes.find({width, height});
    if (it == sizes.end()) return DISP_CHANGE_BADMODE;
    refreshHz = PickRate(it->second, current.refreshHz);
  }
  // Take the driver's own DEVMODE for that mode (it carries fields such as
  // the scaling choice), preferring the default fixed-output variant.
  DEVMODEW chosen{};
  bool found = false;
  for (DWORD i = 0;; ++i) {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsExW(gdiDeviceName.c_str(), i, &dm, 0)) break;
    if (!Usable(dm) || dm.dmPelsWidth != width || dm.dmPelsHeight != height ||
        dm.dmDisplayFrequency != refreshHz)
      continue;
    const bool defaultOutput =
        !(dm.dmFields & DM_DISPLAYFIXEDOUTPUT) || dm.dmDisplayFixedOutput == DMDFO_DEFAULT;
    if (!found || defaultOutput) {
      chosen = dm;
      found = true;
      if (defaultOutput) break;
    }
  }
  if (!found) return DISP_CHANGE_BADMODE;
  chosen.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;
  LONG r = ChangeDisplaySettingsExW(gdiDeviceName.c_str(), &chosen, nullptr, CDS_TEST, nullptr);
  if (r != DISP_CHANGE_SUCCESSFUL) return r;
  return ChangeDisplaySettingsExW(gdiDeviceName.c_str(), &chosen, nullptr, 0, nullptr);
}

LONG RestoreDisplayMode(const std::wstring& gdiDeviceName) {
  if (gdiDeviceName.empty()) return DISP_CHANGE_BADPARAM;
  return ChangeDisplaySettingsExW(gdiDeviceName.c_str(), nullptr, nullptr, 0, nullptr);
}

}  // namespace wvd

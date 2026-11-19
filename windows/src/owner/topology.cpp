#include "topology.h"
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <functional>
#include <set>
#include "wvd/file.h"
#include "wvd/logging.h"

namespace wvd {
namespace {
constexpr uint32_t kSnapMagic = 0x504E5357;
constexpr uint32_t kSnapVersion = 2;
struct SnapshotHeader { uint32_t magic, version, count, entrySize; };
// Persist monitor interface names, never boot-local LUIDs or path indices.
struct SnapshotEntry {
  wchar_t devicePath[128];
  DISPLAYCONFIG_SOURCE_MODE source;
  DISPLAYCONFIG_ROTATION rotation;
  DISPLAYCONFIG_SCALING scaling;
  DISPLAYCONFIG_RATIONAL refresh;
  uint32_t sourceGroup;
};
std::wstring Lower(std::wstring s) {
  for (auto& c : s) c = wchar_t(std::towlower(c));
  return s;
}
bool QuerySdb(UINT32 flags, std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
              std::vector<DISPLAYCONFIG_MODE_INFO>& modes, std::wstring& err) {
  // The topology may change between sizing and querying (hotplug/UAC).
  for (int attempt = 0; attempt < 5; ++attempt) {
    UINT32 pc = 0, mc = 0;
    LONG result = GetDisplayConfigBufferSizes(flags, &pc, &mc);
    if (result == ERROR_SUCCESS) {
      paths.resize(pc); modes.resize(mc);
      result = QueryDisplayConfig(flags, &pc, paths.data(), &mc, modes.data(), nullptr);
      if (result == ERROR_INSUFFICIENT_BUFFER) continue;
      if (result == ERROR_SUCCESS) { paths.resize(pc); modes.resize(mc); return true; }
    }
    err = L"CCD query failed " + std::to_wstring(result) + L" (requires interactive session)";
    return false;
  }
  err = L"display topology kept changing during query";
  return false;
}
DISPLAYCONFIG_TARGET_DEVICE_NAME TargetName(const DISPLAYCONFIG_PATH_INFO& path) {
  DISPLAYCONFIG_TARGET_DEVICE_NAME name{};
  name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
  name.header.size = sizeof(name);
  name.header.adapterId = path.targetInfo.adapterId;
  name.header.id = path.targetInfo.id;
  if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS) name = {};
  return name;
}
bool SameSource(const DISPLAYCONFIG_PATH_INFO& a, const DISPLAYCONFIG_PATH_INFO& b) {
  return a.sourceInfo.id == b.sourceInfo.id &&
         a.sourceInfo.adapterId.HighPart == b.sourceInfo.adapterId.HighPart &&
         a.sourceInfo.adapterId.LowPart == b.sourceInfo.adapterId.LowPart;
}
bool SourceRect(const DISPLAYCONFIG_PATH_INFO& path,
                const std::vector<DISPLAYCONFIG_MODE_INFO>& modes, RECT& rect) {
  const UINT32 index = path.sourceInfo.modeInfoIdx;
  if (!(path.flags & DISPLAYCONFIG_PATH_ACTIVE) || index >= modes.size() ||
      modes[index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) return false;
  const auto& source = modes[index].sourceMode;
  rect = {source.position.x, source.position.y,
          source.position.x + LONG(source.width), source.position.y + LONG(source.height)};
  return true;
}
bool LoadSnapshot(const std::wstring& file, std::vector<SnapshotEntry>& entries,
                   std::wstring& err) {
  std::vector<uint8_t> bytes;
  if (!ReadFileWhole(file, bytes) || bytes.size() < sizeof(SnapshotHeader)) {
    err = L"snapshot missing/unreadable"; return false;
  }
  SnapshotHeader header{};
  memcpy(&header, bytes.data(), sizeof(header));
  if (header.magic != kSnapMagic || header.version != kSnapVersion ||
      header.entrySize != sizeof(SnapshotEntry)) {
    err = L"unsupported topology snapshot; run --save-topology again"; return false;
  }
  if (!header.count || header.count > 64 ||
      bytes.size() != sizeof(header) + size_t(header.count) * sizeof(SnapshotEntry)) {
    err = L"invalid topology snapshot size/count"; return false;
  }
  entries.resize(header.count);
  memcpy(entries.data(), bytes.data() + sizeof(header), entries.size() * sizeof(entries[0]));
  std::set<std::wstring> identities;
  for (const auto& entry : entries) {
    if (!entry.devicePath[0] || entry.devicePath[127] != 0 ||
        !entry.source.width || entry.source.width > 32768 ||
        !entry.source.height || entry.source.height > 32768 ||
        entry.sourceGroup >= entries.size() || !entry.refresh.Denominator ||
        entry.rotation < DISPLAYCONFIG_ROTATION_IDENTITY ||
        entry.rotation > DISPLAYCONFIG_ROTATION_ROTATE270 ||
        !identities.insert(Lower(entry.devicePath)).second) {
      err = L"invalid/duplicate monitor in topology snapshot"; return false;
    }
  }
  return true;
}
}  // namespace

bool CcdEnumerate(std::vector<CcdMonitor>& out, std::wstring& err) {
  out.clear();
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  if (!QuerySdb(QDC_ALL_PATHS, paths, modes, err)) return false;
  for (size_t i = 0; i < paths.size(); ++i) {
    const auto& path = paths[i];
    CcdMonitor monitor;
    monitor.pathId = UINT32(i + 1); monitor.targetId = path.targetInfo.id;
    monitor.activePath = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
    monitor.attachedToTarget = path.targetInfo.targetAvailable != FALSE;
    monitor.friendly = TargetName(path).monitorFriendlyDeviceName;
    RECT rect{};
    if (SourceRect(path, modes, rect)) {
      monitor.x = rect.left; monitor.y = rect.top;
      monitor.width = UINT32(rect.right - rect.left); monitor.height = UINT32(rect.bottom - rect.top);
      monitor.primary = rect.left == 0 && rect.top == 0;
    }
    out.push_back(std::move(monitor));
  }
  return true;
}
bool CcdFindByMatch(const std::wstring& match, CcdMonitor& out, std::wstring& err) {
  std::vector<CcdMonitor> monitors;
  if (!CcdEnumerate(monitors, err)) return false;
  if (!match.empty()) for (const auto& monitor : monitors) {
    if (monitor.activePath && Lower(monitor.friendly).find(Lower(match)) != std::wstring::npos) {
      out = monitor; return true;
    }
  }
  err = L"no active display matches '" + match + L"'"; return false;
}
bool CcdSave(const std::wstring& file, std::wstring& err) {
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  if (!QuerySdb(QDC_ONLY_ACTIVE_PATHS, paths, modes, err)) return false;
  if (paths.empty() || paths.size() > 64) { err = L"invalid active path count"; return false; }
  std::vector<SnapshotEntry> entries;
  for (size_t i = 0; i < paths.size(); ++i) {
    const auto& path = paths[i];
    const auto name = TargetName(path);
    RECT rect{};
    if (!name.monitorDevicePath[0] || !SourceRect(path, modes, rect)) {
      err = L"active display identity/source unavailable; snapshot not saved"; return false;
    }
    SnapshotEntry entry{};
    wcscpy_s(entry.devicePath, name.monitorDevicePath);
    entry.source = modes[path.sourceInfo.modeInfoIdx].sourceMode;
    entry.rotation = path.targetInfo.rotation; entry.scaling = path.targetInfo.scaling;
    entry.refresh = path.targetInfo.refreshRate;
    entry.sourceGroup = uint32_t(i);
    for (size_t j = 0; j < i; ++j) if (SameSource(path, paths[j])) {
      entry.sourceGroup = entries[j].sourceGroup; break;
    }
    entries.push_back(entry);
  }
  SnapshotHeader header{kSnapMagic, kSnapVersion, uint32_t(entries.size()), sizeof(SnapshotEntry)};
  std::vector<uint8_t> bytes(sizeof(header) + entries.size() * sizeof(entries[0]));
  memcpy(bytes.data(), &header, sizeof(header));
  memcpy(bytes.data() + sizeof(header), entries.data(), bytes.size() - sizeof(header));
  if (!WriteFileAtomic(file, bytes.data(), bytes.size())) { err = L"write snapshot failed"; return false; }
  WVD_LOGI("saved %zu active monitor identities to %ls", entries.size(), file.c_str());
  return true;
}
bool CcdRestore(const std::wstring& file, std::wstring& err) {
  std::vector<SnapshotEntry> entries;
  if (!LoadSnapshot(file, entries, err)) return false;
  std::vector<DISPLAYCONFIG_PATH_INFO> candidates, paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> unusedModes, modes;
  if (!QuerySdb(QDC_ALL_PATHS, candidates, unusedModes, err)) return false;
  // Prefer current active routes, then available inactive routes. Resolve IDs
  // from THIS boot, preserving saved clone groups without reusing a source
  // for unrelated monitors. Refuse partial restore if a monitor is unplugged.
  std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
    return bool(a.flags & DISPLAYCONFIG_PATH_ACTIVE) > bool(b.flags & DISPLAYCONFIG_PATH_ACTIVE);
  });
  std::vector<std::wstring> identities;
  for (const auto& candidate : candidates) identities.push_back(Lower(TargetName(candidate).monitorDevicePath));
  std::vector<size_t> routes;
  size_t attempts = 0;
  std::function<bool(size_t)> chooseRoutes = [&](size_t i) {
    if (i == entries.size()) return true;
    if (++attempts > 10000) return false;
    const auto& entry = entries[i];
    for (size_t c = 0; c < candidates.size(); ++c) {
      if (!candidates[c].targetInfo.targetAvailable || identities[c] != Lower(entry.devicePath)) continue;
      bool compatible = true;
      for (size_t j = 0; j < routes.size(); ++j) {
        if (SameSource(candidates[routes[j]], candidates[c]) != (entries[j].sourceGroup == entry.sourceGroup))
          compatible = false;
      }
      if (!compatible) continue;
      routes.push_back(c);
      if (chooseRoutes(i + 1)) return true;
      routes.pop_back();
    }
    return false;
  };
  if (!chooseRoutes(0)) { err = L"saved monitor/route unavailable; topology left intact"; return false; }
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto& entry = entries[i];
    auto path = candidates[routes[i]];
    DISPLAYCONFIG_MODE_INFO mode{};
    mode.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
    mode.adapterId = path.sourceInfo.adapterId; mode.id = path.sourceInfo.id;
    mode.sourceMode = entry.source;
    UINT32 sourceIndex = UINT32(modes.size());
    for (size_t j = 0; j < paths.size(); ++j) if (SameSource(paths[j], path))
      sourceIndex = paths[j].sourceInfo.modeInfoIdx;
    if (sourceIndex == modes.size()) modes.push_back(mode);
    path.sourceInfo.modeInfoIdx = sourceIndex;
    path.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    path.targetInfo.rotation = entry.rotation; path.targetInfo.scaling = entry.scaling;
    path.targetInfo.refreshRate = entry.refresh;
    path.flags = DISPLAYCONFIG_PATH_ACTIVE;
    paths.push_back(path);
  }
  const UINT32 flags = SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES;
  LONG result = SetDisplayConfig(UINT32(paths.size()), paths.data(), UINT32(modes.size()),
                                  modes.data(), flags | SDC_VALIDATE);
  if (result == ERROR_SUCCESS)
    result = SetDisplayConfig(UINT32(paths.size()), paths.data(), UINT32(modes.size()),
                               modes.data(), flags | SDC_APPLY);
  if (result != ERROR_SUCCESS) { err = L"restore SetDisplayConfig failed " + std::to_wstring(result); return false; }
  return true;
}
bool CcdArrangeRight(std::wstring& match, std::wstring& err) {
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  if (!QuerySdb(QDC_ONLY_ACTIVE_PATHS, paths, modes, err)) return false;
  int target = -1, primary = -1;
  RECT primaryRect{};
  for (size_t i = 0; i < paths.size(); ++i) {
    RECT rect{};
    if (!SourceRect(paths[i], modes, rect)) continue;
    if (!match.empty() && Lower(TargetName(paths[i]).monitorFriendlyDeviceName).find(Lower(match)) != std::wstring::npos)
      target = int(i);
    if (!rect.left && !rect.top) { primary = int(i); primaryRect = rect; }
  }
  if (target < 0 || primary < 0 || SameSource(paths[target], paths[primary])) {
    err = L"need a distinct active extended monitor and primary"; return false;
  }
  auto& source = modes[paths[target].sourceInfo.modeInfoIdx].sourceMode;
  source.position = {primaryRect.right, primaryRect.top};
  LONG result = SetDisplayConfig(UINT32(paths.size()), paths.data(), UINT32(modes.size()),
                                  modes.data(), SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_APPLY | SDC_ALLOW_CHANGES);
  if (result != ERROR_SUCCESS) { err = L"arrange SetDisplayConfig failed " + std::to_wstring(result); return false; }
  return true;
}
bool CcdSnapshotLost(const std::wstring& file, std::wstring& err) {
  std::vector<SnapshotEntry> entries;
  if (!LoadSnapshot(file, entries, err)) return false;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  if (!QuerySdb(QDC_ONLY_ACTIVE_PATHS, paths, modes, err)) return false;
  for (const auto& entry : entries) {
    bool found = false;
    for (const auto& path : paths)
      if (Lower(TargetName(path).monitorDevicePath) == Lower(entry.devicePath)) found = true;
    if (!found) { err = L"saved monitor is inactive/missing"; return true; }
  }
  return false;
}
bool CcdMatchesSnapshot(const std::wstring& file, std::wstring& err) {
  std::vector<SnapshotEntry> entries;
  if (!LoadSnapshot(file, entries, err)) return false;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  if (!QuerySdb(QDC_ONLY_ACTIVE_PATHS, paths, modes, err)) return false;
  if (paths.size() != entries.size()) return false;
  for (const auto& entry : entries) {
    bool found = false;
    for (const auto& path : paths) {
      if (Lower(TargetName(path).monitorDevicePath) != Lower(entry.devicePath)) continue;
      RECT rect{};
      if (SourceRect(path, modes, rect) && rect.left == entry.source.position.x &&
          rect.top == entry.source.position.y && UINT32(rect.right - rect.left) == entry.source.width &&
          UINT32(rect.bottom - rect.top) == entry.source.height && path.targetInfo.rotation == entry.rotation)
        found = true;
    }
    if (!found) { err = L"saved monitor layout differs"; return false; }
  }
  return true;
}
}  // namespace wvd

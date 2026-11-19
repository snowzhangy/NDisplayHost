// Topology manager — CCD (Configuration Consolidation & Discovery, §4.2).
// Arrange / persist / crash-safe-restore. Active monitor identity snapshots are
// written atomically (§4.2 "persist, watchdog, restore on start").
#pragma once

#include <string>
#include <vector>

#include "wvd/platform.h"

namespace wvd {

struct CcdMonitor {
  UINT32 targetId = 0;
  UINT32 pathId = 0;
  std::wstring friendly;      // DISPLAYCONFIG_TARGET_DEVICE_NAME.friendlyName
  bool activePath = false;
  bool attachedToTarget = false;
  bool primary = false;
  LONG x = 0, y = 0;          // desktop coords when active
  UINT32 width = 0, height = 0, fps = 0;
  bool builtInTarget = false;
};

bool CcdEnumerate(std::vector<CcdMonitor>& out, std::wstring& err);

// Path id of a target whose friendly name contains `match` (case-insensitive).
bool CcdFindByMatch(const std::wstring& match, CcdMonitor& outMon,
                    std::wstring& err);

// Snapshot active monitor interface identities, source modes and clone groups.
// All CCD operations require an interactive process session, not Session 0.
bool CcdSave(const std::wstring& file, std::wstring& err);

// Restore a snapshot (SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_APPLY).
// Returns false with a message if the config cannot be applied.
bool CcdRestore(const std::wstring& file, std::wstring& err);

// Place the WVD target immediately right of the primary at its native mode.
bool CcdArrangeRight(std::wstring& matchName, std::wstring& err);

// Lost checks identity only; matches also checks position/size/rotation.
bool CcdSnapshotLost(const std::wstring& file, std::wstring& err);
bool CcdMatchesSnapshot(const std::wstring& file, std::wstring& err);

}  // namespace wvd

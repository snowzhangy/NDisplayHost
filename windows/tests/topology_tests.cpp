// Exercise real persistence/remapping code against simulated CCD boots.
#include "../src/owner/topology.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
std::vector<DISPLAYCONFIG_PATH_INFO> fakePaths, appliedPaths;
std::vector<DISPLAYCONFIG_MODE_INFO> fakeModes, appliedModes;
bool resizeRace = false;
int queries = 0, applyCalls = 0;
LONG WINAPI FakeSizes(UINT32 flags, UINT32* pc, UINT32* mc) {
  *pc = UINT32(fakePaths.size()); *mc = UINT32(fakeModes.size());
  CHECK(flags == QDC_ALL_PATHS || flags == QDC_ONLY_ACTIVE_PATHS); return ERROR_SUCCESS;
}
LONG WINAPI FakeQuery(UINT32 flags, UINT32* pc, DISPLAYCONFIG_PATH_INFO* paths,
                      UINT32* mc, DISPLAYCONFIG_MODE_INFO* modes, DISPLAYCONFIG_TOPOLOGY_ID*) {
  ++queries;
  if (resizeRace) { resizeRace = false; return ERROR_INSUFFICIENT_BUFFER; }
  *pc = 0;
  for (const auto& path : fakePaths) {
    if (flags == QDC_ONLY_ACTIVE_PATHS && !(path.flags & DISPLAYCONFIG_PATH_ACTIVE)) continue;
    paths[(*pc)++] = path;
  }
  *mc = UINT32(fakeModes.size());
  if (*mc) memcpy(modes, fakeModes.data(), *mc * sizeof(*modes));
  return ERROR_SUCCESS;
}
LONG WINAPI FakeName(DISPLAYCONFIG_DEVICE_INFO_HEADER* header) {
  auto* name = reinterpret_cast<DISPLAYCONFIG_TARGET_DEVICE_NAME*>(header);
  // Target IDs change between boots; identity stays constant.
  wcscpy_s(name->monitorDevicePath, header->id % 2 ? L"monitor-b" : L"monitor-a");
  return ERROR_SUCCESS;
}
LONG WINAPI FakeSet(UINT32 pc, DISPLAYCONFIG_PATH_INFO* paths, UINT32 mc,
                    DISPLAYCONFIG_MODE_INFO* modes, UINT32 flags) {
  if (flags & SDC_APPLY) {
    ++applyCalls; appliedPaths.assign(paths, paths + pc); appliedModes.assign(modes, modes + mc);
  }
  return ERROR_SUCCESS;
}
#define GetDisplayConfigBufferSizes FakeSizes
#define QueryDisplayConfig FakeQuery
#define DisplayConfigGetDeviceInfo FakeName
#define SetDisplayConfig FakeSet
#include "../src/owner/topology.cpp"
#undef GetDisplayConfigBufferSizes
#undef QueryDisplayConfig
#undef DisplayConfigGetDeviceInfo
#undef SetDisplayConfig

DISPLAYCONFIG_PATH_INFO Path(UINT source, UINT target, UINT index, bool active = true) {
  DISPLAYCONFIG_PATH_INFO path{};
  path.sourceInfo.adapterId.LowPart = 1; path.sourceInfo.id = source;
  path.sourceInfo.modeInfoIdx = index;
  path.targetInfo.adapterId.LowPart = 1; path.targetInfo.id = target;
  path.targetInfo.targetAvailable = TRUE;
  path.targetInfo.rotation = DISPLAYCONFIG_ROTATION_IDENTITY;
  path.targetInfo.scaling = DISPLAYCONFIG_SCALING_IDENTITY;
  path.targetInfo.refreshRate = {60, 1};
  path.flags = active ? DISPLAYCONFIG_PATH_ACTIVE : 0;
  return path;
}
int main() {
  wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
  const std::wstring file = std::wstring(temp) + L"wvd-topology-test-" + std::to_wstring(GetCurrentProcessId()) + L".bin";
  fakePaths = {Path(10, 100, 0), Path(20, 101, 1), Path(30, 999, 0, false)};
  fakeModes.resize(2);
  for (unsigned i = 0; i < 2; ++i) {
    fakeModes[i].infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
    fakeModes[i].sourceMode.width = 1920; fakeModes[i].sourceMode.height = 1080;
    fakeModes[i].sourceMode.position = {LONG(i * 1920), 0};
  }
  std::wstring error;
  resizeRace = true; CHECK(wvd::CcdSave(file, error)); CHECK(queries >= 2);
  // Reboot: both LUIDs and all source/target IDs change. The inactive path
  // must not be part of the snapshot or get activated during restore.
  fakePaths = {Path(77, 900, 0), Path(88, 901, 1)};
  for (auto& path : fakePaths) {
    path.sourceInfo.adapterId.LowPart = path.targetInfo.adapterId.LowPart = 42;
  }
  CHECK(wvd::CcdMatchesSnapshot(file, error));
  CHECK(wvd::CcdRestore(file, error)); CHECK(applyCalls == 1);
  CHECK(appliedPaths.size() == 2 && appliedModes.size() == 2);
  CHECK(appliedPaths[0].sourceInfo.adapterId.LowPart == 42);
  CHECK(appliedPaths[0].targetInfo.id == 900 && appliedPaths[1].targetInfo.id == 901);
  CHECK(appliedModes[1].sourceMode.position.x == 1920);
  fakePaths[1].targetInfo.targetAvailable = FALSE;
  CHECK(!wvd::CcdRestore(file, error)); CHECK(applyCalls == 1);
  fakePaths[1].targetInfo.targetAvailable = TRUE;
  // Greedy routing would steal B's only source. Backtracking must choose A's
  // inactive alternate route on source 88 instead.
  fakePaths = {Path(77, 900, 0), Path(77, 901, 1), Path(88, 900, 0, false)};
  CHECK(wvd::CcdRestore(file, error)); CHECK(applyCalls == 2);
  CHECK(appliedPaths[0].sourceInfo.id == 88 && appliedPaths[1].sourceInfo.id == 77);
  // Legacy, huge, and truncated snapshots fail before allocations/CCD apply.
  const uint32_t invalid[] = {0x504E5357, 1, 0xFFFFFFFF, 0xFFFFFFFF};
  CHECK(wvd::WriteFileAtomic(file, invalid, sizeof(invalid)));
  CHECK(!wvd::CcdRestore(file, error)); CHECK(applyCalls == 2);
  CHECK(wvd::WriteFileAtomic(file, invalid, 4));
  CHECK(!wvd::CcdRestore(file, error)); CHECK(applyCalls == 2);
  DeleteFileW(file.c_str());
  puts("topology reboot, routing, race and corruption regressions passed");
}

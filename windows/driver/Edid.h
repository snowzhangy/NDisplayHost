#pragma once
#include <cstdint>
struct WvdMode { uint32_t width, height, vSync; };
struct WvdEdid { uint8_t data[128]; };
struct WvdTiming { uint32_t hBlank, vBlank; uint64_t pixelRate; };
inline WvdTiming WvdSignalTiming(const WvdMode& mode) {
  const uint64_t rate = uint64_t(mode.width + 160) * (mode.height + 40) * mode.vSync;
  return {160, 40, ((rate + 5000) / 10000) * 10000};
}
extern const WvdMode kWvdReferenceModes[];
extern const unsigned kWvdReferenceModeCount;
constexpr char kWvdMonitorName[] = "WVD Virtual";
constexpr uint32_t kWvdMonitorPpi = 60;
void WvdBuildEdid(WvdEdid& out, const WvdMode& preferred, uint32_t serial,
                  uint32_t ppi, const char* monitorName);

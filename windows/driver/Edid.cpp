#include "Edid.h"
#include <algorithm>
#include <cstring>

// Entry 0 is the preferred mode (EDID detailed timing). The headset's size
// menu offers whatever Windows lists for the monitor, so the wide and
// ultrawide entries are what give it Mac Virtual Display's Standard / Wide /
// Ultrawide choices (Apple: 16:9, 21:9, 32:9 at a fixed height). Sizes over
// the 3840x2160 encode budget are scaled to fit before encoding.
const WvdMode kWvdReferenceModes[] = {
    {3840, 2160, 60}, {3840, 2160, 90}, {2560, 1440, 60},
    {1920, 1080, 60}, {1920, 1080, 90}, {1920, 1080, 120},
    // 21:9
    {5120, 2160, 60}, {3440, 1440, 60}, {2560, 1080, 60},
    // 32:9
    {5120, 1440, 60}, {3840, 1080, 60}};
const unsigned kWvdReferenceModeCount = sizeof(kWvdReferenceModes) / sizeof(kWvdReferenceModes[0]);

void WvdBuildEdid(WvdEdid& out, const WvdMode& mode, uint32_t serial,
                  uint32_t ppi, const char* name) {
  auto* e = out.data;
  memset(e, 0, sizeof(out.data));
  const uint8_t header[] = {0, 255, 255, 255, 255, 255, 255, 0};
  memcpy(e, header, sizeof(header));
  const uint16_t manufacturer = (23 << 10) | (22 << 5) | 4;  // WVD
  e[8] = uint8_t(manufacturer >> 8); e[9] = uint8_t(manufacturer & 0xFF);
  e[10] = 1;
  for (unsigned i = 0; i < 4; ++i) e[12 + i] = uint8_t(serial >> (8 * i));
  e[16] = 1; e[17] = 36; e[18] = 1; e[19] = 4;
  e[20] = 0xA5;
  ppi = (std::max)(1u, ppi);
  const uint32_t widthMm = mode.width * 254 / (10 * ppi);
  const uint32_t heightMm = mode.height * 254 / (10 * ppi);
  e[21] = uint8_t((std::min)(255u, widthMm / 10));
  e[22] = uint8_t((std::min)(255u, heightMm / 10));
  e[23] = 120; e[24] = 0x06;  // gamma 2.2, sRGB, preferred timing
  const uint8_t chroma[] = {0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54};
  memcpy(e + 25, chroma, sizeof(chroma));
  // Eight unused standard timings occupy 38..53. DTDs start at 54.
  for (unsigned i = 38; i < 54; ++i) e[i] = 1;
  auto* d = e + 54;
  const auto timing = WvdSignalTiming(mode);
  const uint32_t clock = uint32_t(timing.pixelRate / 10000);
  d[0] = uint8_t(clock); d[1] = uint8_t(clock >> 8);  // TWO bytes, 10 kHz
  d[2] = uint8_t(mode.width); d[3] = uint8_t(timing.hBlank);
  d[4] = uint8_t(((mode.width >> 8) << 4) | (timing.hBlank >> 8));
  d[5] = uint8_t(mode.height); d[6] = uint8_t(timing.vBlank);
  d[7] = uint8_t(((mode.height >> 8) << 4) | (timing.vBlank >> 8));
  d[8] = 48; d[9] = 32; d[10] = 0x35; d[11] = 0;
  d[12] = uint8_t(widthMm); d[13] = uint8_t(heightMm);
  d[14] = uint8_t(((widthMm >> 8) << 4) | (heightMm >> 8));
  d[17] = 0x1A;  // digital separate sync, +H/-V, progressive
  d = e + 72; d[3] = 0xFC;
  memset(d + 5, ' ', 13);
  const size_t length = (std::min)(size_t(12), strlen(name));
  memcpy(d + 5, name, length); d[5 + length] = '\n';
  e[93] = e[111] = 0x10;  // unused descriptor blocks
  uint8_t checksum = 0;
  for (unsigned i = 0; i < 127; ++i) checksum = uint8_t(checksum + e[i]);
  e[127] = uint8_t(0u - checksum);
}

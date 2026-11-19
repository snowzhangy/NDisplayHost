// Coordinate transforms (§4.4): headset UV → monitor pixels →
// virtual-desktop coordinates. Handles per-monitor DPI (layout coords are
// physical pixels; DPI only labels scale), rotation, and negative origins.
// Recomputed whenever the layout generation changes.
#pragma once

#include <optional>
#include <utility>
#include <vector>

#include "wvd/protocol.h"

namespace wvd {

class LayoutMapper {
 public:
  LayoutMapper() = default;
  explicit LayoutMapper(std::vector<MonitorDesc> monitors) { Update(std::move(monitors)); }

  void Update(std::vector<MonitorDesc> monitors) {
    monitors_ = std::move(monitors);
    // Virtual-desktop bounds including negative origins.
    LONG left = 0, top = 0, right = 0, bottom = 0;
    for (auto& m : monitors_) {
      left = (std::min)(left, LONG(m.x));
      top = (std::min)(top, LONG(m.y));
      right = (std::max)(right, LONG(m.x) + LONG(m.width));
      bottom = (std::max)(bottom, LONG(m.y) + LONG(m.height));
    }
    bounds_[0] = left; bounds_[1] = top; bounds_[2] = right; bounds_[3] = bottom;
  }

  const std::vector<MonitorDesc>& monitors() const { return monitors_; }
  // [left, top, right, bottom] in virtual-desktop pixels.
  const LONG* bounds() const { return bounds_; }
  bool empty() const { return monitors_.empty(); }

  // Headset UV (0..1 on the monitor quad, origin top-left) → virtual-desktop
  // pixel coordinates. Rotation is applied in the direction the layout says
  // (the CCD reports the rotated surface size in MonitorDesc.width/height).
  std::optional<std::pair<int32_t, int32_t>> UvToDesktop(uint16_t monitor,
                                                         float u, float v) const {
    if (monitor >= monitors_.size()) return std::nullopt;
    const MonitorDesc& m = monitors_[monitor];
    float uu = u < 0.f ? 0.f : (u > 1.f ? 1.f : u);
    float vv = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    // UV → unrotated surface coords:
    float sx = uu, sy = vv;
    switch (m.rotation) {
      case 90:  std::swap(sx, sy); sx = 1.f - sx; break;
      case 180: sx = 1.f - sx; sy = 1.f - sy; break;
      case 270: std::swap(sx, sy); sy = 1.f - sy; break;
      default: break;
    }
    int32_t px = m.x + static_cast<int32_t>(sx * (m.width - 1) + 0.5f);
    int32_t py = m.y + static_cast<int32_t>(sy * (m.height - 1) + 0.5f);
    return std::make_pair(px, py);
  }

  // Virtual-desktop pixel → SendInput absolute coordinates (normalized
  // 0..65535 across the union of all monitors, VIRTUALDESK semantics).
  std::optional<std::pair<LONG, LONG>> DesktopToSendInput(int32_t x, int32_t y) const {
    if (monitors_.empty()) return std::nullopt;
    LONG w = bounds_[2] - bounds_[0];
    LONG h = bounds_[3] - bounds_[1];
    if (w <= 1 || h <= 1) return std::nullopt;
    LONG nx = ((x - bounds_[0]) * 65535L) / (w - 1);
    LONG ny = ((y - bounds_[1]) * 65535L) / (h - 1);
    return std::make_pair(nx, ny);
  }

 private:
  std::vector<MonitorDesc> monitors_;
  LONG bounds_[4] = {0, 0, 0, 0};
};

}  // namespace wvd

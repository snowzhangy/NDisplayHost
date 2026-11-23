// Embedded game depth (GAME_DEPTH_HOST_PLAN.md §3–6, WIRE.md §6.1).
//
// Depth is not an alpha channel and not a second stream: it is a grayscale
// strip drawn under the colour in the SAME coded picture, so colour and depth
// belong to one frame, arrive together and are lost together. The strip
// carries inverse depth (larger = nearer) as 8-bit full-range luma, normalised
// by the host (percentiles + smoothing live in the media session, §5).
//
// This header is the single source of the geometry; the packed
// EmbeddedDepthPayload mirrors wvd.proto EmbeddedDepthLayout field-for-field
// (the QUIC adapter maps it onto the protobuf message).
#pragma once

#include <cstdint>

namespace wvd {

// DepthEncoding (proto): inverse depth, larger = nearer, host-normalised.
enum DepthEncoding : uint32_t {
  kDepthInverseNearWhite = 0,
};

// Geometry of one coded picture with an embedded depth strip. Derived purely
// from the colour size, so the host and the headset agree without negotiation
// beyond CodecConfig itself.
struct EmbeddedDepthPayload {
  uint32_t pictureWidth;   // colour rect at (0,0) of the coded picture
  uint32_t pictureHeight;
  uint32_t stripY;         // first row of the depth strip
  uint32_t mapWidth;
  uint32_t mapHeight;
  uint32_t slices;         // horizontal bands, side by side in the strip
  uint32_t encoding;       // DepthEncoding
  // Host-side derived values (not on the wire; the client derives sliceRows
  // itself):
  uint32_t sliceRows;      // mapHeight / slices
  uint32_t codedHeight;    // align_up(stripY + sliceRows, 8)
};

inline constexpr uint32_t kDepthAlignUp8(uint32_t v) { return (v + 7u) & ~7u; }

// Computes the layout for a colour picture of W×H (the ENCODED colour size,
// which is what the headset compares against the map shape). Plan §4:
//
//   map_width  = floor(W/4)            slices = 4
//   map_height = floor(H/16)*4         (divisible by slices)
//   strip_y    = align_up(H,8) + 16    (16 guard rows under the colour)
//   coded_h    = align_up(strip_y + slice_rows, 8)
//
// Returns false for sizes too small or oddly shaped to carry a map.
inline bool ComputeEmbeddedDepth(uint32_t w, uint32_t h, EmbeddedDepthPayload& out) {
  out = {};
  if (w < 256 || h < 128) return false;
  out.pictureWidth = w;
  out.pictureHeight = h;
  out.mapWidth = w / 4u;
  out.slices = 4u;
  out.mapHeight = (h / 16u) * 4u;
  if (out.mapWidth == 0 || out.mapHeight < out.slices) return false;
  out.sliceRows = out.mapHeight / out.slices;
  out.stripY = kDepthAlignUp8(h) + 16u;
  out.codedHeight = kDepthAlignUp8(out.stripY + out.sliceRows);
  out.encoding = kDepthInverseNearWhite;
  return true;
}

}  // namespace wvd

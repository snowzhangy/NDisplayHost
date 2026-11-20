#pragma once

#include "wvd/protocol.h"

namespace wvd {

// Visit the NAL units without their three- or four-byte Annex B start codes.
template <class Visitor>
void ForEachAnnexBNal(const uint8_t* data, size_t size, Visitor&& visit) {
  auto findStart = [&](size_t from, size_t& codeSize) {
    for (size_t i = from; size - i >= 3; ++i) {
      if (data[i] != 0 || data[i + 1] != 0) continue;
      if (data[i + 2] == 1) {
        codeSize = 3;
        return i;
      }
      if (size - i >= 4 && data[i + 2] == 0 && data[i + 3] == 1) {
        codeSize = 4;
        return i;
      }
    }
    return size;
  };
  size_t codeSize = 0;
  size_t start = findStart(0, codeSize);
  while (start < size) {
    const size_t begin = start + codeSize;
    start = findStart(begin, codeSize);
    if (begin < start) visit(data + begin, start - begin);
  }
}

inline int AnnexBNalType(Codec codec, uint8_t firstByte) {
  return codec == Codec::H264 ? (firstByte & 0x1F) : ((firstByte >> 1) & 0x3F);
}

inline bool IsAnnexBParameterSet(Codec codec, int type) {
  if (codec == Codec::H264) return type == 7 || type == 8;
  return type >= 32 && type <= 34;
}

}  // namespace wvd

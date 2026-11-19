#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace wvd {

// In-process boundary between capture/encode and the WIRE v1 session.
class MediaChannel {
 public:
  virtual ~MediaChannel() = default;
  virtual bool Send(const uint8_t* data, size_t len) = 0;
  bool SendPacket(const std::vector<uint8_t>& packet) { return Send(packet.data(), packet.size()); }
  virtual std::optional<std::vector<uint8_t>> RecvPacket(uint32_t timeoutMs) = 0;
  virtual size_t BacklogBytes() const = 0;
  virtual uint32_t VideoBitrateLimitKbps() const = 0;
  virtual void Close() = 0;
  virtual bool IsConnected() const = 0;
};

}  // namespace wvd

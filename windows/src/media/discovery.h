#pragma once

#include <cstdint>
#include <string>

namespace wvd {

// One _wvd._udp registration for the lifetime of the media worker.
class DnsRegistration {
 public:
  ~DnsRegistration();
  bool Start(const std::wstring& instanceName, uint16_t port, const std::string& hostId);

 private:
  void* registration_ = nullptr;
};

}  // namespace wvd

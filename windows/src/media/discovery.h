#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "wvd/platform.h"

namespace wvd {

struct DnsRegistrationCtx;

class DnsRegistration {
 public:
  DnsRegistration();
  ~DnsRegistration();
  void Start(const std::wstring& instanceName, uint16_t port, const std::string& hostId);
  void Refresh();

 private:
  std::unique_ptr<DnsRegistrationCtx> registration_;
  HANDLE interfaceChange_ = nullptr, addressChange_ = nullptr;
  std::atomic<bool> dirty_{true};
  uint64_t nextScan_ = 0, nextRetry_ = 0;
  uint32_t address_ = 0, interfaceIndex_ = 0;
  uint16_t port_ = 0;
  std::wstring instanceFq_, hostName_, hostId_;
};

}  // namespace wvd

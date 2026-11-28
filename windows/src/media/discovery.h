#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "wvd/platform.h"

namespace wvd {

struct DnsRegistrationCtx;

// Advertises _wvd._udp on the best LAN interface and, when this PC is running
// a direct-connect hotspot (Mobile Hotspot or Wi-Fi Direct legacy AP), on that
// virtual adapter too, so a headset joined to the PC's own network finds it.
class DnsRegistration {
 public:
  DnsRegistration();
  ~DnsRegistration();
  void Start(const std::wstring& instanceName, uint16_t port, const std::string& hostId);
  void Refresh();

  // One DNS-SD registration bound to one interface.
  struct Advertiser {
    ~Advertiser();
    std::unique_ptr<DnsRegistrationCtx> registration;
    uint64_t nextRetry = 0;
    uint32_t address = 0, interfaceIndex = 0;
    std::wstring instanceFq;
    // Used instead of instanceFq if Windows rejects the shared name.
    std::wstring fallbackFq;
  };

 private:
  void Pump(Advertiser& advertiser, uint64_t now);
  Advertiser lan_, direct_;
  HANDLE interfaceChange_ = nullptr, addressChange_ = nullptr;
  std::atomic<bool> dirty_{true};
  uint64_t nextScan_ = 0;
  uint16_t port_ = 0;
  std::wstring hostName_, hostId_;
};

}  // namespace wvd

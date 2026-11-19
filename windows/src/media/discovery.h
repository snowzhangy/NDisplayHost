// DNS-SD registration via native DnsServiceRegister (§4.2, WIRE.md §2).
//
// The service instance follows the transport, and both may be up at once:
//   * `_wvd._tcp`  — the packed TCP host the current headset builds use;
//   * `_wvd._udp`  — the QUIC host (WIRE.md v1), TXT `v=1` and `id=<host id>`.
// Advertising one transport while speaking the other makes the discovered
// endpoint unusable, so each listener registers exactly what it serves.
//
// The registration lives only as long as the calling process — the media
// worker owns it. Treat it as a hint only; pairing always authenticates with
// the pinned key (§4.7-7).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wvd/platform.h"

namespace wvd {

class DnsRegistration {
 public:
  DnsRegistration() = default;
  ~DnsRegistration() { StopAll(); }

  // instanceName e.g. "WVM-PC"; registers <instance>._wvd._tcp.local on `port`.
  bool Start(const std::wstring& instanceName, uint16_t port);
  // QUIC service: <instance>._wvd._udp.local with TXT id=<hostId>, v=1.
  bool StartUdp(const std::wstring& instanceName, uint16_t port, const std::string& hostId);

  void StopTcp();
  void StopUdp();
  void StopAll();
  // Stops everything (kept under the original name for existing callers).
  void Stop() { StopAll(); }
  bool active() const { return !registrations_.empty(); }

 private:
  bool Register(const std::wstring& instanceName, const wchar_t* serviceName, uint16_t port,
                const std::vector<std::pair<std::string, std::string>>& txt, bool udp);
  void StopOne(bool udp);

  // Opaque DNS_SERVICE_REGISTER_REQUEST contexts, tagged by transport.
  struct Entry {
    void* ctx = nullptr;
    bool udp = false;
  };
  std::vector<Entry> registrations_;
};

}  // namespace wvd

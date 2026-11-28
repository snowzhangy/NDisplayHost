#pragma once
// Locating the PC's own access point for direct connect.
//
// Both Windows Mobile Hotspot and the Wi-Fi Direct legacy AP run on a
// "Microsoft Wi-Fi Direct Virtual Adapter": a software 802.11 interface whose
// address comes from the Internet Connection Sharing DHCP scope
// (192.168.137.1/24 unless an administrator changed ScopeAddress). The adapter
// description is localized, so the ICS scope is the primary signal.
#include "wvd/platform.h"

#include <ws2ipdef.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")

namespace wvd {

struct DirectLink {
  uint32_t address = 0;   // network byte order; 0 = none
  uint32_t ifIndex = 0;
};

// The ICS scope address in network byte order (default 192.168.137.1).
inline uint32_t IcsScopeAddress() {
  wchar_t text[64]{};
  DWORD size = sizeof(text);
  IN_ADDR parsed{};
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\SharedAccess\\Parameters",
                   L"ScopeAddress", RRF_RT_REG_SZ, nullptr, text, &size) == ERROR_SUCCESS &&
      InetPtonW(AF_INET, text, &parsed) == 1)
    return parsed.S_un.S_addr;
  InetPtonW(AF_INET, L"192.168.137.1", &parsed);
  return parsed.S_un.S_addr;
}

inline bool InIcsScope(uint32_t address, uint32_t scope) {
  const uint32_t mask = htonl(0xFFFFFF00u);  // ICS always hands out a /24
  return (address & mask) == (scope & mask);
}

inline std::string FormatIpv4(uint32_t address) {
  IN_ADDR ip{};
  ip.S_un.S_addr = address;
  const auto& b = ip.S_un.S_un_b;
  char text[16];
  snprintf(text, sizeof(text), "%u.%u.%u.%u", unsigned(b.s_b1), unsigned(b.s_b2),
           unsigned(b.s_b3), unsigned(b.s_b4));
  return text;
}

// The up Wi-Fi interface that hosts this PC's access point. ICS always gives
// the hosting interface exactly the scope address, so that is the signal.
// Drivers differ in what they expose: inbox ones use a software "Microsoft
// Wi-Fi Direct Virtual Adapter", while e.g. the MediaTek MT7925 reports the
// hotspot as a second *hardware* port named after the card itself. A non-
// hardware "Wi-Fi Direct" adapter with another address is the last resort.
inline DirectLink FindDirectLink() {
  ULONG size = 15000;
  std::vector<uint8_t> data(size);
  const auto adapters = [&] { return reinterpret_cast<PIP_ADAPTER_ADDRESSES>(data.data()); };
  constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
  DWORD result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters(), &size);
  for (int retry = 0; result == ERROR_BUFFER_OVERFLOW && retry < 2; ++retry) {
    data.resize(size);
    result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters(), &size);
  }
  DirectLink scoped, named;
  if (result != ERROR_SUCCESS) return scoped;
  const uint32_t scope = IcsScopeAddress();
  for (auto* adapter = adapters(); adapter; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp || adapter->IfType != IF_TYPE_IEEE80211) continue;
    MIB_IF_ROW2 row{};
    row.InterfaceLuid = adapter->Luid;
    if (GetIfEntry2(&row) != NO_ERROR) continue;
    const bool wifiDirect = !row.InterfaceAndOperStatusFlags.HardwareInterface &&
                            adapter->Description && wcsstr(adapter->Description, L"Wi-Fi Direct");
    for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
      if (unicast->DadState != IpDadStatePreferred || !unicast->Address.lpSockaddr ||
          unicast->Address.lpSockaddr->sa_family != AF_INET) continue;
      const uint32_t ip =
          reinterpret_cast<const SOCKADDR_IN*>(unicast->Address.lpSockaddr)->sin_addr.S_un.S_addr;
      const uint8_t first = static_cast<uint8_t>(ip & 0xFF);
      if (first == 0 || first == 127 || first >= 224 || (ip & 0xFFFF) == 0xFEA9) continue;  // 169.254/16
      // Exactly the scope address: a PC that is itself a *client* of another
      // ICS hotspot has some other 192.168.137.x and must not match.
      if (ip == scope && !scoped.address) scoped = {ip, adapter->IfIndex};
      else if (wifiDirect && !named.address) named = {ip, adapter->IfIndex};
      break;
    }
  }
  return scoped.address ? scoped : named;
}

}  // namespace wvd

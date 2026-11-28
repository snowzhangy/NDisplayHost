#include "discovery.h"

#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <windns.h>

#include <cstdio>
#include <vector>

#include "wvd/direct_link.h"
#include "wvd/logging.h"

#pragma comment(lib, "dnsapi.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace wvd {

struct DnsRegistrationCtx {
  ~DnsRegistrationCtx() { if (instance) DnsServiceFreeInstance(instance); }
  DNS_SERVICE_REGISTER_REQUEST request{};
  PDNS_SERVICE_INSTANCE instance = nullptr;
  std::atomic<DWORD> result{DNS_REQUEST_PENDING};
  bool retiring = false;
  bool logged = false;
  IP4_ADDRESS address = 0;
};

namespace {

struct LanAddress {
  IP4_ADDRESS address = 0;
  ULONG index = 0;
  ULONG metric = 0;
  bool gateway = false;
};

bool UsableAddress(const SOCKET_ADDRESS& socket) {
  if (!socket.lpSockaddr || socket.iSockaddrLength < sizeof(SOCKADDR_IN) ||
      socket.lpSockaddr->sa_family != AF_INET) return false;
  const auto& ip = reinterpret_cast<const SOCKADDR_IN*>(socket.lpSockaddr)->sin_addr.S_un.S_un_b;
  return ip.s_b1 != 0 && ip.s_b1 != 127 && ip.s_b1 < 224 &&
         !(ip.s_b1 == 169 && ip.s_b2 == 254);
}

LanAddress SelectLanAddress() {
  ULONG size = 15000;
  std::vector<uint8_t> data(size);
  const auto adapters = [&] { return reinterpret_cast<PIP_ADAPTER_ADDRESSES>(data.data()); };
  constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                          GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
  DWORD result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters(), &size);
  for (int retry = 0; result == ERROR_BUFFER_OVERFLOW && retry < 2; ++retry) {
    data.resize(size);
    result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters(), &size);
  }
  LanAddress best;
  if (result != ERROR_SUCCESS) return best;
  // Some drivers (MT7925) expose this PC's own hotspot as a hardware Wi-Fi
  // port; it is advertised separately as the direct link, never as the LAN.
  const uint32_t hotspot = IcsScopeAddress();
  for (auto* adapter = adapters(); adapter; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp ||
        (adapter->IfType != IF_TYPE_ETHERNET_CSMACD && adapter->IfType != IF_TYPE_IEEE80211))
      continue;
    MIB_IF_ROW2 iface{};
    iface.InterfaceLuid = adapter->Luid;
    if (GetIfEntry2(&iface) != NO_ERROR || !iface.InterfaceAndOperStatusFlags.HardwareInterface ||
        iface.AccessType != NET_IF_ACCESS_BROADCAST) continue;
    bool gateway = false;
    for (auto* next = adapter->FirstGatewayAddress; next; next = next->Next)
      gateway |= UsableAddress(next->Address);
    for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
      if (unicast->DadState != IpDadStatePreferred || !UsableAddress(unicast->Address)) continue;
      const auto* ip = reinterpret_cast<const SOCKADDR_IN*>(unicast->Address.lpSockaddr);
      if (ip->sin_addr.S_un.S_addr == hotspot) break;
      if (!best.address || (gateway && !best.gateway) ||
          (gateway == best.gateway && adapter->Ipv4Metric < best.metric))
        best = {ip->sin_addr.S_un.S_addr, adapter->IfIndex, adapter->Ipv4Metric, gateway};
      break;
    }
  }
  return best;
}

void WINAPI RegisterComplete(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance) {
  if (instance) DnsServiceFreeInstance(instance);
  static_cast<DnsRegistrationCtx*>(context)->result = status;
}

void WINAPI InterfaceChanged(PVOID context, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE) {
  *static_cast<std::atomic<bool>*>(context) = true;
}

void WINAPI AddressChanged(PVOID context, PMIB_UNICASTIPADDRESS_ROW, MIB_NOTIFICATION_TYPE) {
  *static_cast<std::atomic<bool>*>(context) = true;
}

void Deregister(DnsRegistrationCtx& entry) {
  entry.retiring = true;
  entry.result = DNS_REQUEST_PENDING;
  const DWORD result = DnsServiceDeRegister(&entry.request, nullptr);
  if (result != DNS_REQUEST_PENDING) entry.result = result;
}

// Prints the address line the control panel parses; `label` is the prefix.
void Announce(const char* label, uint32_t address) {
  if (address) printf("%s %s\n", label, FormatIpv4(address).c_str());
  else printf("%s unavailable\n", label);
  fflush(stdout);
}

// Adjusts the address/interface an advertiser should serve; it re-registers on
// the next Pump when they changed.
bool Retarget(DnsRegistration::Advertiser& advertiser, uint32_t address, uint32_t index) {
  if (address == advertiser.address && index == advertiser.interfaceIndex) return false;
  advertiser.address = address;
  advertiser.interfaceIndex = index;
  advertiser.nextRetry = 0;
  return true;
}

}  // namespace

DnsRegistration::Advertiser::~Advertiser() {
  if (registration && !registration->retiring && registration->result == ERROR_SUCCESS)
    Deregister(*registration);
  // DNSAPI can finish after the worker exits this scope. The OS withdraws any
  // registration that is still in flight at process exit.
  if (registration && registration->result == DNS_REQUEST_PENDING) registration.release();
}

DnsRegistration::DnsRegistration() = default;

void DnsRegistration::Start(const std::wstring& instanceName, uint16_t port,
                            const std::string& hostId) {
  lan_.instanceFq = instanceName + L"._wvd._udp.local.";
  // The same name on the PC's own access point. A headset is only ever on one
  // of the two links, and Vision Pro reconnects to the Bonjour *name* it last
  // used: a headset that moves from the router to direct connect mid-session
  // then finds this PC again by itself. Should Windows refuse a second
  // registration of the name, fall back to a distinct one.
  direct_.instanceFq = lan_.instanceFq;
  std::wstring directName = instanceName;
  if (directName.size() > 50) directName.resize(50);
  direct_.fallbackFq = directName + L" (Direct)._wvd._udp.local.";
  wchar_t name[256]{};
  DWORD length = ARRAYSIZE(name);
  GetComputerNameExW(ComputerNameDnsHostname, name, &length);
  hostName_ = std::wstring(name[0] ? name : L"localhost") + L".local.";
  hostId_ = Utf8ToWide(hostId.c_str());
  port_ = port;
  NotifyIpInterfaceChange(AF_INET, InterfaceChanged, &dirty_, FALSE, &interfaceChange_);
  NotifyUnicastIpAddressChange(AF_INET, AddressChanged, &dirty_, FALSE, &addressChange_);
  Refresh();
}

void DnsRegistration::Refresh() {
  if (!port_) return;
  const uint64_t now = GetTickCount64();
  if (dirty_.exchange(false) || now >= nextScan_) {
    const bool first = nextScan_ == 0;
    nextScan_ = now + 5000;  // covers a route/metric change or unavailable notifications
    const auto lan = SelectLanAddress();
    if (Retarget(lan_, lan.address, lan.index) || first) Announce("HOST_LAN_ADDRESS", lan_.address);
    const auto link = FindDirectLink();
    const bool hadDirect = direct_.address != 0;
    if (Retarget(direct_, link.address, link.ifIndex) && (link.address || hadDirect))
      Announce("HOST_DIRECT_ADDRESS", direct_.address);
  }
  Pump(lan_, now);
  Pump(direct_, now);
}

void DnsRegistration::Pump(Advertiser& advertiser, uint64_t now) {
  if (advertiser.registration) {
    auto& entry = *advertiser.registration;
    const DWORD result = entry.result;
    if (result == DNS_REQUEST_PENDING) return;
    if (entry.retiring || result != ERROR_SUCCESS) {
      if (result != ERROR_SUCCESS) {
        WVD_LOGW("DNS-SD %s of %ls failed (0x%lx)",
                 entry.retiring ? "deregistration" : "registration",
                 advertiser.instanceFq.c_str(), result);
        advertiser.nextRetry = now + 5000;
        if (!entry.retiring && !advertiser.fallbackFq.empty()) {
          advertiser.instanceFq = advertiser.fallbackFq;
          advertiser.fallbackFq.clear();
          advertiser.nextRetry = now;  // retry at once under the distinct name
        }
      }
      advertiser.registration.reset();
    } else if (entry.address != advertiser.address ||
               entry.request.InterfaceIndex != advertiser.interfaceIndex) {
      Deregister(entry);
      return;
    } else if (!entry.logged) {
      WVD_LOGI("DNS-SD registered %ls on interface %u", advertiser.instanceFq.c_str(),
               advertiser.interfaceIndex);
      entry.logged = true;
    }
  }
  if (advertiser.registration || !advertiser.address || now < advertiser.nextRetry) return;

  auto entry = std::make_unique<DnsRegistrationCtx>();
  entry->address = advertiser.address;
  PCWSTR keys[] = {L"v", L"id"};
  PCWSTR values[] = {L"1", hostId_.c_str()};
  entry->instance = DnsServiceConstructInstance(advertiser.instanceFq.c_str(), hostName_.c_str(),
      &entry->address, nullptr, port_, 0, 0, ARRAYSIZE(keys), keys, values);
  if (!entry->instance) {
    WVD_LOGW("DNS-SD could not construct the service instance");
    advertiser.nextRetry = now + 5000;
    return;
  }
  entry->request.Version = DNS_QUERY_REQUEST_VERSION1;
  entry->request.InterfaceIndex = advertiser.interfaceIndex;
  entry->request.pServiceInstance = entry->instance;
  entry->request.pRegisterCompletionCallback = RegisterComplete;
  entry->request.pQueryContext = entry.get();
  const DWORD result = DnsServiceRegister(&entry->request, nullptr);
  if (result != DNS_REQUEST_PENDING) entry->result = result;
  advertiser.registration = std::move(entry);
}

DnsRegistration::~DnsRegistration() {
  if (interfaceChange_) CancelMibChangeNotify2(interfaceChange_);
  if (addressChange_) CancelMibChangeNotify2(addressChange_);
  // lan_ and direct_ withdraw their registrations in ~Advertiser.
}

}  // namespace wvd

#include "discovery.h"

#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <windns.h>

#include <cstdio>
#include <vector>

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

}  // namespace

DnsRegistration::DnsRegistration() = default;

void DnsRegistration::Start(const std::wstring& instanceName, uint16_t port,
                            const std::string& hostId) {
  instanceFq_ = instanceName + L"._wvd._udp.local.";
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
    if (first || lan.address != address_ || lan.index != interfaceIndex_) {
      address_ = lan.address;
      interfaceIndex_ = lan.index;
      nextRetry_ = 0;
      if (address_) {
        IN_ADDR ip{};
        ip.S_un.S_addr = address_;
        const auto& b = ip.S_un.S_un_b;
        printf("HOST_LAN_ADDRESS %u.%u.%u.%u\n", unsigned(b.s_b1), unsigned(b.s_b2),
               unsigned(b.s_b3), unsigned(b.s_b4));
      } else {
        puts("HOST_LAN_ADDRESS unavailable");
      }
      fflush(stdout);
    }
  }

  if (registration_) {
    auto& entry = *registration_;
    const DWORD result = entry.result;
    if (result == DNS_REQUEST_PENDING) return;
    if (entry.retiring || result != ERROR_SUCCESS) {
      if (result != ERROR_SUCCESS) {
        WVD_LOGW("DNS-SD %s failed (0x%lx)",
                 entry.retiring ? "deregistration" : "registration", result);
        nextRetry_ = now + 5000;
      }
      registration_.reset();
    } else if (entry.address != address_ || entry.request.InterfaceIndex != interfaceIndex_) {
      Deregister(entry);
      return;
    } else if (!entry.logged) {
      WVD_LOGI("DNS-SD registered %ls on interface %u", instanceFq_.c_str(), interfaceIndex_);
      entry.logged = true;
    }
  }
  if (registration_ || !address_ || now < nextRetry_) return;

  auto entry = std::make_unique<DnsRegistrationCtx>();
  entry->address = address_;
  PCWSTR keys[] = {L"v", L"id"};
  PCWSTR values[] = {L"1", hostId_.c_str()};
  entry->instance = DnsServiceConstructInstance(instanceFq_.c_str(), hostName_.c_str(),
      &entry->address, nullptr, port_, 0, 0, ARRAYSIZE(keys), keys, values);
  if (!entry->instance) {
    WVD_LOGW("DNS-SD could not construct the service instance");
    nextRetry_ = now + 5000;
    return;
  }
  entry->request.Version = DNS_QUERY_REQUEST_VERSION1;
  entry->request.InterfaceIndex = interfaceIndex_;
  entry->request.pServiceInstance = entry->instance;
  entry->request.pRegisterCompletionCallback = RegisterComplete;
  entry->request.pQueryContext = entry.get();
  const DWORD result = DnsServiceRegister(&entry->request, nullptr);
  if (result != DNS_REQUEST_PENDING) entry->result = result;
  registration_ = std::move(entry);
}

DnsRegistration::~DnsRegistration() {
  if (interfaceChange_) CancelMibChangeNotify2(interfaceChange_);
  if (addressChange_) CancelMibChangeNotify2(addressChange_);
  if (registration_ && !registration_->retiring && registration_->result == ERROR_SUCCESS)
    Deregister(*registration_);
  // DNSAPI can finish after the worker exits this scope. The OS withdraws any
  // registration that is still in flight at process exit.
  if (registration_ && registration_->result == DNS_REQUEST_PENDING) registration_.release();
}

}  // namespace wvd

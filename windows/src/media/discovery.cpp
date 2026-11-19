#include "discovery.h"
#include "wvd/platform.h"

#include <iphlpapi.h>
#include <windns.h>
#include <ws2ipdef.h>

#include <atomic>
#include <string>
#include <vector>

#include "wvd/logging.h"

#pragma comment(lib, "dnsapi.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace wvd {

// SDK shape (windns.h, 10.0.26100): the register request carries the
// instance only; Stop() calls DnsServiceDeRegister with the same request.
struct DnsRegistrationCtx {
  DNS_SERVICE_REGISTER_REQUEST req{};
  PDNS_SERVICE_INSTANCE inst = nullptr;
  std::wstring instanceFq;  // "<name>._wvd._udp.local."
  std::wstring hostName;    // DNS host name of this PC
  IP4_ADDRESS ip4 = 0;      // network-byte-order IPv4
  // msdns calls our completion for the register *and* for the de-register, and
  // it may still do so after a call reported a failure. Until that callback
  // has run the request, the instance and this struct are not ours to free.
  std::atomic<bool> registered{false};
  std::atomic<bool> retiring{false};
};

static VOID WINAPI RegisterComplete(DWORD status, PVOID ctx, PDNS_SERVICE_INSTANCE instance) {
  if (instance) DnsServiceFreeInstance(instance);
  auto* c = static_cast<DnsRegistrationCtx*>(ctx);
  if (!c) return;
  if (c->retiring.exchange(false)) return;  // the answer to our own de-register
  if (status == 0)
    WVD_LOGI("DNS-SD: registered %ls", c->instanceFq.c_str());
  else
    WVD_LOGW("DNS-SD register completion status=0x%lx (hint only, §4.7-7)", status);
}

static bool FirstRoutableIpv4(IP4_ADDRESS& out) {
  ULONG len = 15000;
  std::vector<uint8_t> bytes(len);
  auto adapters = [&] { return reinterpret_cast<PIP_ADAPTER_ADDRESSES>(bytes.data()); };
  constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
  DWORD status = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters(), &len);
  if (status == ERROR_BUFFER_OVERFLOW) {
    bytes.resize(len);
    status = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters(), &len);
  }
  if (status != ERROR_SUCCESS) return false;
  for (auto* adapter = adapters(); adapter; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
        adapter->IfType == IF_TYPE_TUNNEL) continue;
    for (auto* ua = adapter->FirstUnicastAddress; ua; ua = ua->Next) {
      const auto* address = reinterpret_cast<const SOCKADDR_IN*>(ua->Address.lpSockaddr);
      if (!address || address->sin_family != AF_INET) continue;
      out = address->sin_addr.S_un.S_addr;
      return true;
    }
  }
  return false;
}

bool DnsRegistration::Start(const std::wstring& instanceName, uint16_t port,
                            const std::string& hostId) {
  if (registration_) return false;
  auto* c = new DnsRegistrationCtx();
  c->instanceFq = instanceName + L"._wvd._udp.local.";

  wchar_t host[256] = {};
  DWORD hl = 256;
  GetComputerNameExW(ComputerNameDnsHostname, host, &hl);
  c->hostName = std::wstring(host[0] ? host : L"localhost") + L".local.";

  if (!FirstRoutableIpv4(c->ip4))
    WVD_LOGW("DNS-SD: no routable IPv4 found; registering with empty address");

  const auto id = Utf8ToWide(hostId.c_str());
  PCWSTR keys[] = {L"v", L"id"};
  PCWSTR values[] = {L"1", id.c_str()};
  c->inst = DnsServiceConstructInstance(c->instanceFq.c_str(), c->hostName.c_str(),
                                        c->ip4 ? &c->ip4 : nullptr, nullptr,
                                        port, 0, 0, ARRAYSIZE(keys), keys, values);
  if (!c->inst) {
    WVD_LOGE("DnsServiceConstructInstance failed");
    delete c;
    return false;
  }

  c->req.Version = 1;
  c->req.InterfaceIndex = 0;
  c->req.pServiceInstance = c->inst;
  c->req.pRegisterCompletionCallback = RegisterComplete;
  c->req.pQueryContext = c;
  c->req.hCredentials = nullptr;
  c->req.unicastEnabled = FALSE;

  const DWORD status = DnsServiceRegister(&c->req, nullptr);
  registration_ = c;
  c->registered = status == ERROR_SUCCESS || status == DNS_REQUEST_PENDING;
  if (!c->registered)
    WVD_LOGW("DNS-SD registration failed (status 0x%lx)", status);
  return c->registered;
}

DnsRegistration::~DnsRegistration() {
  auto* c = static_cast<DnsRegistrationCtx*>(registration_);
  if (!c || !c->registered) return;
  c->retiring = true;
  const DWORD status = DnsServiceDeRegister(&c->req, nullptr);
  if (status != ERROR_SUCCESS && status != DNS_REQUEST_PENDING)
    WVD_LOGW("DNS-SD deregistration failed (status 0x%lx)", status);
  // The sole registration's request and instance stay valid for the process.
  // DNSAPI can complete deregistration after this destructor returns.
}

}  // namespace wvd

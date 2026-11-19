#include "discovery.h"

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

// Both transports, per WIRE.md §2 and the headset's browser.
static constexpr wchar_t kTcpService[] = L"_wvd._tcp.local.";
static constexpr wchar_t kUdpService[] = L"_wvd._udp.local.";

static VOID WINAPI RegisterComplete(DWORD status, PVOID ctx, PDNS_SERVICE_INSTANCE) {
  auto* c = static_cast<DnsRegistrationCtx*>(ctx);
  if (!c) return;
  if (c->retiring.exchange(false)) return;  // the answer to our own de-register
  if (status == 0)
    WVD_LOGI("DNS-SD: registered %ls", c->instanceFq.c_str());
  else
    WVD_LOGW("DNS-SD register completion status=0x%lx (hint only, §4.7-7)", status);
}

static bool FirstRoutableIpv4(IP4_ADDRESS& out) {
  ULONG len = 8 * 1024;
  PIP_ADAPTER_ADDRESSES addrs = static_cast<PIP_ADAPTER_ADDRESSES>(malloc(len));
  bool found = false;
  if (addrs && GetAdaptersAddresses(AF_INET,
                                    GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr,
                                    addrs, &len) == ERROR_SUCCESS) {
    for (auto* a = addrs; a && !found; a = a->Next) {
      if (a->OperStatus != IfOperStatusUp) continue;
      for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
        auto* in = reinterpret_cast<SOCKADDR_IN*>(ua->Address.lpSockaddr);
        if (in->sin_family != AF_INET) continue;
        ULONG addr = in->sin_addr.S_un.S_addr;
        if ((addr & 0xFF000000u) == 0x7F000000u) continue;  // loopback
        out = addr;  // already network byte order
        found = true;
        break;
      }
    }
  }
  if (addrs) free(addrs);
  return found;
}

bool DnsRegistration::Register(const std::wstring& instanceName, const wchar_t* serviceName,
                               uint16_t port,
                               const std::vector<std::pair<std::string, std::string>>& txt,
                               bool udp) {
  (void)txt;  // DnsServiceConstructInstance takes no TXT records (dwProperties
              // must be 0), so the hint carries the port only. The host id and
              // protocol version travel in the authenticated HelloAck instead.
  for (auto& e : registrations_) {
    if (e.udp == udp) {
      StopOne(udp);
      break;
    }
  }
  auto* c = new DnsRegistrationCtx();
  c->instanceFq = instanceName + L"." + serviceName;

  wchar_t host[256] = {};
  DWORD hl = 256;
  GetComputerNameExW(ComputerNameDnsHostname, host, &hl);
  c->hostName = host[0] ? host : L"localhost";

  if (!FirstRoutableIpv4(c->ip4))
    WVD_LOGW("DNS-SD: no routable IPv4 found; registering with empty address");

  c->inst = DnsServiceConstructInstance(c->instanceFq.c_str(), c->hostName.c_str(), &c->ip4,
                                        nullptr, port, 0 /*prio*/, 0 /*weight*/, 0, nullptr,
                                        nullptr);
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

  DWORD st = DnsServiceRegister(&c->req, nullptr);
  if (st != 0 && st != ERROR_IO_PENDING) {
    WVD_LOGW("DnsServiceRegister status=0x%lx (discovery is a hint only, §4.7-7)", st);
    // The completion callback still owns this context, so it is kept rather
    // than freed; it is never de-registered because nothing was registered.
    // Discovery is a hint, and the contexts are bounded by the two transports
    // this process announces.
    c->registered = false;
    registrations_.push_back(Entry{c, udp});
    return false;
  }
  c->registered = true;
  registrations_.push_back(Entry{c, udp});
  WVD_LOGI("DNS-SD announcing %ls on port %u", c->instanceFq.c_str(), port);
  return true;
}

bool DnsRegistration::Start(const std::wstring& instanceName, uint16_t port) {
  return Register(instanceName, kTcpService, port, {}, false);
}

bool DnsRegistration::StartUdp(const std::wstring& instanceName, uint16_t port,
                               const std::string& hostId) {
  std::vector<std::pair<std::string, std::string>> txt = {
      {"v", "1"},
      {"id", hostId},
  };
  return Register(instanceName, kUdpService, port, txt, true);
}

void DnsRegistration::StopOne(bool udp) {
  for (auto it = registrations_.begin(); it != registrations_.end(); ++it) {
    if (it->udp != udp) continue;
    auto* c = static_cast<DnsRegistrationCtx*>(it->ctx);
    if (c && c->registered) {
      c->retiring = true;
      const DWORD st = DnsServiceDeRegister(&c->req, nullptr);
      if (st != 0 && st != ERROR_IO_PENDING) {
        WVD_LOGW("DnsServiceDeRegister status=0x%lx", st);
        c->retiring = false;
      }
    }
    // Neither the instance nor the context is freed: see DnsRegistrationCtx.
    registrations_.erase(it);
    return;
  }
}

void DnsRegistration::StopTcp() { StopOne(false); }
void DnsRegistration::StopUdp() { StopOne(true); }

void DnsRegistration::StopAll() {
  while (!registrations_.empty()) StopOne(registrations_.front().udp);
}

}  // namespace wvd

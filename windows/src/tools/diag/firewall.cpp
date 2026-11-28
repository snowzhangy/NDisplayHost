#include "wvd/platform.h"
#include "wvd/direct_link.h"

#include <netfw.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>

namespace wvd {
namespace {

using Microsoft::WRL::ComPtr;
constexpr long kProfiles = NET_FW_PROFILE2_PRIVATE | NET_FW_PROFILE2_DOMAIN;
// The PC's own access point (Mobile Hotspot / Wi-Fi Direct) is an unidentified
// network, which Windows Firewall treats as Public. This second rule admits it
// on every profile but only from the ICS DHCP scope and only on Wi-Fi.
constexpr long kDirectProfiles = NET_FW_PROFILE2_ALL;

struct Bstr {
  Bstr() = default;
  explicit Bstr(const wchar_t* value) : value(SysAllocString(value)) {}
  ~Bstr() { SysFreeString(value); }
  BSTR value = nullptr;
};

struct RuleSpec {
  std::wstring name, remote, interfaceTypes;
  long profiles;
};

// Firewall normalizes "a.b.c.0/24" to "a.b.c.0/255.255.255.0"; accept either.
bool SameRemote(const wchar_t* actual, const std::wstring& expected) {
  if (_wcsicmp(actual, expected.c_str()) == 0) return true;
  const auto slash = expected.find(L'/');
  if (slash == std::wstring::npos || expected.compare(slash, std::wstring::npos, L"/24") != 0) return false;
  return _wcsicmp(actual, (expected.substr(0, slash) + L"/255.255.255.0").c_str()) == 0;
}

bool RuleMatches(INetFwRule* rule, const wchar_t* path, const RuleSpec& spec) {
  Bstr application, ports, remote, interfaces;
  long protocol = 0, profiles = 0;
  VARIANT_BOOL enabled = VARIANT_FALSE;
  NET_FW_RULE_DIRECTION direction{};
  NET_FW_ACTION action{};
  if (!spec.interfaceTypes.empty() &&
      !(SUCCEEDED(rule->get_InterfaceTypes(&interfaces.value)) && interfaces.value &&
        _wcsicmp(interfaces.value, spec.interfaceTypes.c_str()) == 0)) return false;
  return SUCCEEDED(rule->get_ApplicationName(&application.value)) && application.value &&
      _wcsicmp(application.value, path) == 0 &&
      SUCCEEDED(rule->get_LocalPorts(&ports.value)) && ports.value && wcscmp(ports.value, L"47800") == 0 &&
      SUCCEEDED(rule->get_RemoteAddresses(&remote.value)) && remote.value &&
      SameRemote(remote.value, spec.remote) &&
      SUCCEEDED(rule->get_Protocol(&protocol)) && protocol == NET_FW_IP_PROTOCOL_UDP &&
      SUCCEEDED(rule->get_Profiles(&profiles)) && profiles == spec.profiles &&
      SUCCEEDED(rule->get_Direction(&direction)) && direction == NET_FW_RULE_DIR_IN &&
      SUCCEEDED(rule->get_Action(&action)) && action == NET_FW_ACTION_ALLOW &&
      SUCCEEDED(rule->get_Enabled(&enabled)) && enabled == VARIANT_TRUE;
}

// Returns S_OK with present=true when the rule exists (or was just added).
HRESULT EnsureRule(INetFwRules* rules, const wchar_t* path, const RuleSpec& spec, bool install,
                   bool& present) {
  present = false;
  Bstr ruleName(spec.name.c_str());
  ComPtr<INetFwRule> existing;
  if (SUCCEEDED(rules->Item(ruleName.value, &existing)) && RuleMatches(existing.Get(), path, spec)) {
    present = true;
    return S_OK;
  }
  if (!install) return S_OK;
  // A same-named rule with other settings (older build, changed ICS scope) is replaced.
  if (existing) rules->Remove(ruleName.value);
  ComPtr<INetFwRule> rule;
  HRESULT hr = CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&rule));
  if (FAILED(hr)) return hr;
  Bstr application(path), ports(L"47800"), remote(spec.remote.c_str()),
       interfaces(spec.interfaceTypes.empty() ? L"All" : spec.interfaceTypes.c_str());
  if (FAILED(hr = rule->put_Name(ruleName.value)) ||
      FAILED(hr = rule->put_ApplicationName(application.value)) ||
      FAILED(hr = rule->put_Protocol(NET_FW_IP_PROTOCOL_UDP)) ||
      FAILED(hr = rule->put_LocalPorts(ports.value)) ||
      FAILED(hr = rule->put_RemoteAddresses(remote.value)) ||
      FAILED(hr = rule->put_InterfaceTypes(interfaces.value)) ||
      FAILED(hr = rule->put_Profiles(spec.profiles)) ||
      FAILED(hr = rule->put_Direction(NET_FW_RULE_DIR_IN)) ||
      FAILED(hr = rule->put_Action(NET_FW_ACTION_ALLOW)) ||
      FAILED(hr = rule->put_Enabled(VARIANT_TRUE)) ||
      FAILED(hr = rules->Add(rule.Get()))) return hr;
  present = true;
  return S_OK;
}

HRESULT InspectFirewall(bool install) {
  wchar_t path[32768]{};
  const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
  if (!length || length == ARRAYSIZE(path)) return E_FAIL;
  // Stable name per extracted location so different copies can coexist.
  uint64_t pathHash = 14695981039346656037ull;
  for (DWORD i = 0; i < length; ++i) {
    const wchar_t folded = path[i] >= L'A' && path[i] <= L'Z' ? path[i] + L'a' - L'A' : path[i];
    pathHash = (pathHash ^ folded) * 1099511628211ull;
  }
  const std::wstring suffix = L"(" + std::to_wstring(pathHash) + L")";
  ComPtr<INetFwPolicy2> policy;
  HRESULT hr = CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&policy));
  if (FAILED(hr)) return hr;
  long activeProfiles = 0;
  if (SUCCEEDED(policy->get_CurrentProfileTypes(&activeProfiles)) &&
      (activeProfiles & NET_FW_PROFILE2_PUBLIC))
    puts("FIREWALL_PUBLIC");
  ComPtr<INetFwRules> rules;
  if (FAILED(hr = policy->get_Rules(&rules))) return hr;

  const RuleSpec lan{L"NDisplayHostWin UDP 47800 " + suffix, L"LocalSubnet", L"", kProfiles};
  const uint32_t scope = IcsScopeAddress() & htonl(0xFFFFFF00u);
  const RuleSpec direct{L"NDisplayHostWin direct connect UDP 47800 " + suffix,
                        Utf8ToWide(FormatIpv4(scope).c_str()) + L"/24", L"Wireless", kDirectProfiles};
  bool present = false;
  if (FAILED(hr = EnsureRule(rules.Get(), path, lan, install, present))) return hr;
  puts(present ? "FIREWALL_RULE_PRESENT" : "FIREWALL_RULE_MISSING");
  if (FAILED(hr = EnsureRule(rules.Get(), path, direct, install, present))) return hr;
  puts(present ? "FIREWALL_DIRECT_RULE_PRESENT" : "FIREWALL_DIRECT_RULE_MISSING");
  return S_OK;
}

}  // namespace

int DiagFirewall(bool install) {
  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(initialized)) return 1;
  const HRESULT hr = InspectFirewall(install);
  CoUninitialize();
  if (SUCCEEDED(hr)) return 0;
  fprintf(stderr, "Windows Firewall rule %s failed (0x%08lx).\n",
          install ? "installation" : "check", static_cast<unsigned long>(hr));
  return 1;
}

}  // namespace wvd

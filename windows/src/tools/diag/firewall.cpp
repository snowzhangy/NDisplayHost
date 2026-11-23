#include "wvd/platform.h"

#include <netfw.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>

namespace wvd {
namespace {

using Microsoft::WRL::ComPtr;
constexpr long kProfiles = NET_FW_PROFILE2_PRIVATE | NET_FW_PROFILE2_DOMAIN;

struct Bstr {
  Bstr() = default;
  explicit Bstr(const wchar_t* value) : value(SysAllocString(value)) {}
  ~Bstr() { SysFreeString(value); }
  BSTR value = nullptr;
};

bool RuleMatches(INetFwRule* rule, const wchar_t* path) {
  Bstr application, ports, remote;
  long protocol = 0, profiles = 0;
  VARIANT_BOOL enabled = VARIANT_FALSE;
  NET_FW_RULE_DIRECTION direction{};
  NET_FW_ACTION action{};
  return SUCCEEDED(rule->get_ApplicationName(&application.value)) && application.value &&
      _wcsicmp(application.value, path) == 0 &&
      SUCCEEDED(rule->get_LocalPorts(&ports.value)) && ports.value && wcscmp(ports.value, L"47800") == 0 &&
      SUCCEEDED(rule->get_RemoteAddresses(&remote.value)) && remote.value &&
      _wcsicmp(remote.value, L"LocalSubnet") == 0 &&
      SUCCEEDED(rule->get_Protocol(&protocol)) && protocol == NET_FW_IP_PROTOCOL_UDP &&
      SUCCEEDED(rule->get_Profiles(&profiles)) && profiles == kProfiles &&
      SUCCEEDED(rule->get_Direction(&direction)) && direction == NET_FW_RULE_DIR_IN &&
      SUCCEEDED(rule->get_Action(&action)) && action == NET_FW_ACTION_ALLOW &&
      SUCCEEDED(rule->get_Enabled(&enabled)) && enabled == VARIANT_TRUE;
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
  const std::wstring name = L"NDisplayHostWin UDP 47800 (" + std::to_wstring(pathHash) + L")";
  Bstr ruleName(name.c_str());
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
  ComPtr<INetFwRule> existing;
  if (SUCCEEDED(rules->Item(ruleName.value, &existing)) && RuleMatches(existing.Get(), path)) {
    puts("FIREWALL_RULE_PRESENT");
    return S_OK;
  }
  if (!install) {
    puts("FIREWALL_RULE_MISSING");
    return S_OK;
  }
  ComPtr<INetFwRule> rule;
  if (FAILED(hr = CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&rule)))) return hr;
  Bstr application(path), ports(L"47800"), remote(L"LocalSubnet");
  if (FAILED(hr = rule->put_Name(ruleName.value)) ||
      FAILED(hr = rule->put_ApplicationName(application.value)) ||
      FAILED(hr = rule->put_Protocol(NET_FW_IP_PROTOCOL_UDP)) ||
      FAILED(hr = rule->put_LocalPorts(ports.value)) ||
      FAILED(hr = rule->put_RemoteAddresses(remote.value)) ||
      FAILED(hr = rule->put_Profiles(kProfiles)) ||
      FAILED(hr = rule->put_Direction(NET_FW_RULE_DIR_IN)) ||
      FAILED(hr = rule->put_Action(NET_FW_ACTION_ALLOW)) ||
      FAILED(hr = rule->put_Enabled(VARIANT_TRUE)) ||
      FAILED(hr = rules->Add(rule.Get()))) return hr;
  puts("FIREWALL_RULE_PRESENT");
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

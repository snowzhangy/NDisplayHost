// "network" role: an offline access point using the public Wi-Fi Direct
// legacy-AP desktop API.
// Legacy mode lets ordinary Wi-Fi clients join; no internet uplink is needed.
#include "wvd/platform.h"
#include "wvd/roles.h"
#include "wvd/stop_signal.h"
#include <wlanapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.WiFiDirect.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Security.Credentials.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <memory>
#include <vector>

using namespace winrt;
using namespace Windows::Devices::WiFiDirect;
namespace {
std::atomic<bool> stopping{false}, started{false}, failed{false};
struct PeerState {
  std::mutex mutex;
  std::vector<WiFiDirectDevice> devices;
  bool closing = false;
};
BOOL WINAPI StopHandler(DWORD) { stopping = true; return TRUE; }
Windows::Foundation::IAsyncAction AcceptPeer(WiFiDirectConnectionRequest request, std::shared_ptr<PeerState> state) {
  try {
    auto device = co_await WiFiDirectDevice::FromIdAsync(request.DeviceInformation().Id());
    if (!device) co_return;
    std::lock_guard<std::mutex> guard(state->mutex);
    for (auto iterator = state->devices.begin(); iterator != state->devices.end();) {
      if (iterator->ConnectionStatus() != WiFiDirectConnectionStatus::Connected) {
        iterator->Close(); iterator = state->devices.erase(iterator);
      } else ++iterator;
    }
    if (!state->closing && state->devices.size() < 8) {
      state->devices.push_back(device);
      printf("PEER_CONNECTED\n"); fflush(stdout);
      for (const auto& endpoint : device.GetConnectionEndpointPairs()) {
        if (endpoint.LocalHostName().Type() != Windows::Networking::HostNameType::Ipv4) continue;
        printf("DIRECT_WIFI_ADDRESS=%s\n", to_string(endpoint.LocalHostName().CanonicalName()).c_str());
        fflush(stdout);
        break;
      }
    } else device.Close();
  } catch (const hresult_error& error) {
    fprintf(stderr, "Peer connection failed: %ls\n", error.message().c_str());
  }
}
int Probe() {
  HANDLE client = nullptr;
  DWORD version = 0;
  if (WlanOpenHandle(2, nullptr, &version, &client) != ERROR_SUCCESS) return 1;
  PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
  const auto result = WlanEnumInterfaces(client, nullptr, &interfaces);
  if (result == ERROR_SUCCESS) for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i)
    wprintf(L"WIFI_ADAPTER %ls\n", interfaces->InterfaceInfo[i].strInterfaceDescription);
  const bool found = result == ERROR_SUCCESS && interfaces->dwNumberOfItems > 0;
  if (interfaces) WlanFreeMemory(interfaces);
  WlanCloseHandle(client, nullptr);
  if (found) puts("DIRECT_WIFI_REQUIRES_START_CHECK");
  else puts("NO_WIFI_ADAPTER");
  return found ? 0 : 1;
}
}  // namespace

namespace wvd {
int RoleNetwork(int argc, wchar_t** argv) {
  std::wstring ssid, password, stopEvent;
  bool probe = false;
  for (int i = 1; i < argc; ++i) {
    const std::wstring argument = argv[i];
    if (argument == L"--probe") probe = true;
    else if (argument == L"--ssid" && i + 1 < argc) ssid = argv[++i];
    else if (argument == L"--stop-event" && i + 1 < argc) stopEvent = argv[++i];
    else return 2;
  }
  if (probe) return Probe();  // never activates or changes the radio
  // Pass the password on redirected stdin, never command line or logs.
  wchar_t secret[128]{};
  if (!fgetws(secret, ARRAYSIZE(secret), stdin)) return 2;
  password = secret;
  SecureZeroMemory(secret, sizeof(secret));
  while (!password.empty() && (password.back() == L'\n' || password.back() == L'\r')) password.pop_back();
  if (ssid.empty() || wvd::WideToUtf8(ssid.c_str()).size() > 32 || password.size() < 8 || password.size() > 63)
    return 2;
  for (auto c : password) if (c < 32 || c > 126) return 2;
  wvd::StopSignal stopSignal;
  if (!stopSignal.Watch(stopEvent, stopping)) return 1;
  if (stopping) return 0;
  SetConsoleCtrlHandler(StopHandler, TRUE);
  try {
    init_apartment(apartment_type::multi_threaded);
    WiFiDirectAdvertisementPublisher publisher;
    WiFiDirectConnectionListener listener;
    auto peers = std::make_shared<PeerState>();
    auto advertisement = publisher.Advertisement();
    advertisement.IsAutonomousGroupOwnerEnabled(true);
    auto settings = advertisement.LegacySettings();
    settings.IsEnabled(true);
    settings.Ssid(ssid);
    Windows::Security::Credentials::PasswordCredential credential;
    credential.Password(password);
    settings.Passphrase(credential);
    SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
    password.clear();
    auto connectionSubscription = listener.ConnectionRequested(auto_revoke, [peers](auto const&, WiFiDirectConnectionRequestedEventArgs const& args) {
      AcceptPeer(args.GetConnectionRequest(), peers);
    });
    auto statusSubscription = publisher.StatusChanged(auto_revoke, [](auto const&, WiFiDirectAdvertisementPublisherStatusChangedEventArgs const& args) {
      switch (args.Status()) {
        case WiFiDirectAdvertisementPublisherStatus::Started:
          started = true; puts("DIRECT_WIFI_STARTED"); break;
        case WiFiDirectAdvertisementPublisherStatus::Aborted:
          failed = true; fprintf(stderr, "DIRECT_WIFI_FAILED error=%d\n", int(args.Error())); break;
        case WiFiDirectAdvertisementPublisherStatus::Stopped:
          if (!failed) puts("DIRECT_WIFI_STOPPED");
          if (!stopping) failed = true; break;
        default: break;
      }
      fflush(stdout);
    });
    if (stopping) return 0;
    publisher.Start();
    const auto deadline = GetTickCount64() + 15000;
    while (!stopping && !failed && (!started || GetTickCount64() < deadline)) {
      if (!started && GetTickCount64() >= deadline) {
        fprintf(stderr, "DIRECT_WIFI_FAILED startup timed out\n");
        failed = true; break;
      }
      Sleep(100);
      if (started) break;
    }
    while (started && !stopping && !failed) Sleep(100);
    publisher.Stop();
    std::lock_guard<std::mutex> guard(peers->mutex);
    peers->closing = true;
    for (auto& peer : peers->devices) peer.Close();
    peers->devices.clear();
    return failed ? 1 : 0;
  } catch (const hresult_error& error) {
    fprintf(stderr, "DIRECT_WIFI_FAILED 0x%08X: %ls\n", unsigned(error.code()), error.message().c_str());
    return 1;
  }
}
}  // namespace wvd

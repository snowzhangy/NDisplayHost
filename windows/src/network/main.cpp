// "network" role: Direct connect — this PC becomes the access point Vision Pro
// joins instead of a router.
//
// Two backends, tried in this order with --mode auto:
//   hotspot  Windows Mobile Hotspot (NetworkOperatorTetheringManager). Picks
//            the band (5 GHz by default), runs DHCP, and shares the PC's own
//            uplink (Ethernet or its Wi-Fi to a router) with the headset
//            through Internet Connection Sharing, so the headset keeps
//            internet access. Needs some connected network to share from.
//   legacy   Wi-Fi Direct legacy-AP publisher. Works with no network at all
//            (airplane mode with Wi-Fi on) but is local-only, and Windows
//            chooses the channel.
//
// stdout protocol for the control panel (one line each):
//   DIRECT_WIFI_MODE=hotspot|legacy   DIRECT_WIFI_STARTED   DIRECT_WIFI_STOPPED
//   DIRECT_WIFI_BAND=auto|2.4|5|6     DIRECT_WIFI_ADDRESS=a.b.c.d
//   DIRECT_WIFI_INTERNET=shared <name> | none
//   DIRECT_WIFI_CLIENTS=<n>           DIRECT_WIFI_ADOPTED=<ssid>
//   DIRECT_WIFI_STATION_MHZ=<mhz>     DIRECT_WIFI_NOTE <text>
// Failures go to stderr as "DIRECT_WIFI_FAILED <reason>".
// The password arrives on stdin and is never printed.
#include "wvd/platform.h"
#include "wvd/direct_link.h"
#include "wvd/roles.h"
#include "wvd/stop_signal.h"
#include "wvd/wlan_streaming.h"
#include <wlanapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.WiFiDirect.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Networking.NetworkOperators.h>
#include <winrt/Windows.Security.Credentials.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <memory>
#include <optional>
#include <vector>

using namespace winrt;
using namespace Windows::Devices::WiFiDirect;
namespace Connectivity = Windows::Networking::Connectivity;
namespace Operators = Windows::Networking::NetworkOperators;

namespace {
std::atomic<bool> stopping{false}, started{false}, failed{false};
BOOL WINAPI StopHandler(DWORD) { stopping = true; return TRUE; }
// Held while the network is up: no background scans pulling the shared radio
// off the headset's channel (see wvd/wlan_streaming.h). Reverted on exit.
wvd::WlanStreamingMode radio;

void Out(const char* format, ...) {
  va_list args;
  va_start(args, format);
  vprintf(format, args);
  va_end(args);
  putchar('\n');
  fflush(stdout);
}
void Fail(const char* format, ...) {
  fputs("DIRECT_WIFI_FAILED ", stderr);
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputc('\n', stderr);
  fflush(stderr);
}
// Sleeps in short steps so a stop request is honoured promptly.
bool Wait(DWORD milliseconds) {
  const auto end = GetTickCount64() + milliseconds;
  while (!stopping && GetTickCount64() < end) Sleep(50);
  return !stopping;
}

// ---- Station link -----------------------------------------------------------
// Frequency of the PC's own Wi-Fi connection to a router, 0 when none. With a
// single-channel radio the access point must share this channel, so a 2.4 GHz
// router link drags the headset onto 2.4 GHz too.
ULONG StationMhz() {
  HANDLE client = nullptr;
  DWORD version = 0;
  if (WlanOpenHandle(2, nullptr, &version, &client) != ERROR_SUCCESS) return 0;
  ULONG mhz = 0;
  PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
  if (WlanEnumInterfaces(client, nullptr, &interfaces) == ERROR_SUCCESS) {
    for (DWORD i = 0; i < interfaces->dwNumberOfItems && !mhz; ++i) {
      // Note: a channel number alone cannot tell 6 GHz ch1-13 from 2.4 GHz;
      // 6 GHz links are rare on PCs and the BSS frequency corrects it when available.
      const auto& info = interfaces->InterfaceInfo[i];
      if (info.isState != wlan_interface_state_connected) continue;
      DWORD size = 0;
      PWLAN_CONNECTION_ATTRIBUTES connection = nullptr;
      if (WlanQueryInterface(client, &info.InterfaceGuid, wlan_intf_opcode_current_connection, nullptr,
                             &size, reinterpret_cast<PVOID*>(&connection), nullptr) != ERROR_SUCCESS)
        continue;
      auto& association = connection->wlanAssociationAttributes;
      // The BSS list (exact frequency) is empty unless the user granted
      // desktop apps location access; the channel number is always readable.
      PULONG channel = nullptr;
      if (WlanQueryInterface(client, &info.InterfaceGuid, wlan_intf_opcode_channel_number, nullptr,
                             &size, reinterpret_cast<PVOID*>(&channel), nullptr) == ERROR_SUCCESS) {
        if (*channel >= 1 && *channel <= 14) mhz = *channel == 14 ? 2484 : 2407 + 5 * *channel;
        else if (*channel >= 32 && *channel <= 177) mhz = 5000 + 5 * *channel;
        WlanFreeMemory(channel);
      }
      PWLAN_BSS_LIST list = nullptr;
      if (WlanGetNetworkBssList(client, &info.InterfaceGuid, &association.dot11Ssid,
                                association.dot11BssType, TRUE, nullptr, &list) == ERROR_SUCCESS) {
        for (DWORD b = 0; b < list->dwNumberOfItems; ++b) {
          if (memcmp(list->wlanBssEntries[b].dot11Bssid, association.dot11Bssid, 6) == 0) {
            mhz = list->wlanBssEntries[b].ulChCenterFrequency / 1000;  // kHz -> MHz, exact (incl. 6 GHz)
            break;
          }
        }
        WlanFreeMemory(list);
      }
      WlanFreeMemory(connection);
    }
    WlanFreeMemory(interfaces);
  }
  WlanCloseHandle(client, nullptr);
  return mhz;
}

void ReportStation() {
  const ULONG mhz = StationMhz();
  if (!mhz) return;
  Out("DIRECT_WIFI_STATION_MHZ=%lu", mhz);
  if (mhz < 3000)
    Out("DIRECT_WIFI_NOTE This PC's Wi-Fi is connected to a 2.4 GHz network. The headset network "
        "shares that channel; use Ethernet or a 5 GHz network on the PC for smooth streaming.");
}

// Reports the PC's access-point address once its virtual adapter is up.
void ReportAddress(DWORD waitMs) {
  const auto end = GetTickCount64() + waitMs;
  for (;;) {
    const auto link = wvd::FindDirectLink();
    if (link.address) { Out("DIRECT_WIFI_ADDRESS=%s", wvd::FormatIpv4(link.address).c_str()); return; }
    if (stopping || GetTickCount64() >= end) return;
    Sleep(250);
  }
}

// ---- Mobile Hotspot -------------------------------------------------------------
const char* CapabilityName(Operators::TetheringCapability capability) {
  switch (capability) {
    case Operators::TetheringCapability::Enabled: return "enabled";
    case Operators::TetheringCapability::DisabledByGroupPolicy: return "disabled by group policy";
    case Operators::TetheringCapability::DisabledByHardwareLimitation: return "not supported by this Wi-Fi adapter";
    case Operators::TetheringCapability::DisabledByOperator: return "disabled by the network operator";
    case Operators::TetheringCapability::DisabledBySku: return "not available in this Windows edition";
    case Operators::TetheringCapability::DisabledByRequiredAppNotInstalled: return "requires an operator app";
    case Operators::TetheringCapability::DisabledBySystemCapability: return "disabled by a system capability";
    default: return "unavailable";
  }
}

const char* StatusName(Operators::TetheringOperationStatus status) {
  using S = Operators::TetheringOperationStatus;
  switch (status) {
    case S::Success: return "success";
    case S::WiFiDeviceOff: return "Wi-Fi is turned off";
    case S::MobileBroadbandDeviceOff: return "the mobile broadband device is off";
    case S::OperationInProgress: return "another hotspot operation is in progress";
    case S::BluetoothDeviceOff: return "Bluetooth is off";
    case S::NetworkLimitedConnectivity: return "the shared network has limited connectivity";
    case S::AlreadyOn: return "Mobile Hotspot is already on";
    case S::RadioRestriction: return "the radio is restricted (airplane mode or regulatory limits)";
    case S::BandInterference: return "the requested band conflicts with this PC's current Wi-Fi connection";
    case S::EntitlementCheckFailure:
    case S::EntitlementCheckTimeout: return "the operator entitlement check failed";
    default: return "Windows reported an unknown error";
  }
}

struct Profile {
  Connectivity::ConnectionProfile profile{nullptr};
  bool internet = false;
};

// The connection whose traffic the hotspot shares. Internet first; otherwise
// any connected profile Windows will tether from (e.g. a LAN with no internet).
std::optional<Profile> PickProfile(std::string& reason) {
  reason = "this PC has no connected network to share (use --mode legacy for a local-only network)";
  std::vector<Connectivity::ConnectionProfile> candidates;
  if (auto internet = Connectivity::NetworkInformation::GetInternetConnectionProfile()) candidates.push_back(internet);
  for (const auto& profile : Connectivity::NetworkInformation::GetConnectionProfiles()) candidates.push_back(profile);
  for (const auto& profile : candidates) {
    try {
      if (!profile.NetworkAdapter() ||
          profile.GetNetworkConnectivityLevel() == Connectivity::NetworkConnectivityLevel::None) continue;
      const auto capability =
          Operators::NetworkOperatorTetheringManager::GetTetheringCapabilityFromConnectionProfile(profile);
      if (capability != Operators::TetheringCapability::Enabled) {
        reason = std::string("Mobile Hotspot is ") + CapabilityName(capability);
        continue;
      }
      const bool internet = profile.GetNetworkConnectivityLevel() ==
                            Connectivity::NetworkConnectivityLevel::InternetAccess;
      return Profile{profile, internet};
    } catch (const hresult_error&) {
    }
  }
  return std::nullopt;
}

Operators::TetheringWiFiBand ParseBand(const std::wstring& band) {
  if (band == L"2.4") return Operators::TetheringWiFiBand::TwoPointFourGigahertz;
  if (band == L"5") return Operators::TetheringWiFiBand::FiveGigahertz;
  if (band == L"6") return Operators::TetheringWiFiBand::SixGigahertz;
  return Operators::TetheringWiFiBand::Auto;
}
const char* BandName(Operators::TetheringWiFiBand band) {
  switch (band) {
    case Operators::TetheringWiFiBand::TwoPointFourGigahertz: return "2.4";
    case Operators::TetheringWiFiBand::FiveGigahertz: return "5";
    case Operators::TetheringWiFiBand::SixGigahertz: return "6";
    default: return "auto";
  }
}

enum class Outcome { Stopped, Failed, Unavailable };

// Starts Mobile Hotspot with this session's SSID/password, keeps it up until a
// stop request, then turns it off and puts back anything it changed.
Outcome RunHotspot(const std::wstring& ssid, const std::wstring& password, Operators::TetheringWiFiBand band) {
  std::string reason;
  const auto source = PickProfile(reason);
  if (!source) { Out("DIRECT_WIFI_NOTE Mobile Hotspot unavailable: %s", reason.c_str()); return Outcome::Unavailable; }
  Operators::NetworkOperatorTetheringManager manager{nullptr};
  try {
    manager = Operators::NetworkOperatorTetheringManager::CreateFromConnectionProfile(source->profile);
  } catch (const hresult_error& error) {
    Out("DIRECT_WIFI_NOTE Mobile Hotspot unavailable: %ls", error.message().c_str());
    return Outcome::Unavailable;
  }
  const std::string sourceName = to_string(source->profile.ProfileName());

  // Someone already turned Mobile Hotspot on: use it as it is and leave it on.
  if (manager.TetheringOperationalState() == Operators::TetheringOperationalState::On) {
    const auto current = manager.GetCurrentAccessPointConfiguration();
    Out("DIRECT_WIFI_MODE=hotspot");
    Out("DIRECT_WIFI_ADOPTED=%s", to_string(current.Ssid()).c_str());
    Out("DIRECT_WIFI_INTERNET=%s %s", source->internet ? "shared" : "local", sourceName.c_str());
    Out("DIRECT_WIFI_STARTED");
    Out("DIRECT_WIFI_RADIO streaming-mode=%s", radio.Engage() ? "on" : "unavailable");
    ReportAddress(5000);
    uint32_t clients = UINT32_MAX;
    while (Wait(1000) && manager.TetheringOperationalState() != Operators::TetheringOperationalState::Off) {
      if (manager.ClientCount() != clients) Out("DIRECT_WIFI_CLIENTS=%u", clients = manager.ClientCount());
    }
    if (!stopping) Out("DIRECT_WIFI_NOTE Mobile Hotspot was turned off in Windows.");
    Out("DIRECT_WIFI_STOPPED");
    return Outcome::Stopped;
  }

  // Band support comes from the adapter the manager will use.
  const auto current = manager.GetCurrentAccessPointConfiguration();
  if (band != Operators::TetheringWiFiBand::Auto) {
    bool supported = false;
    try { supported = current.IsBandSupported(band); } catch (const hresult_error&) {}
    if (!supported) {
      Out("DIRECT_WIFI_NOTE This Wi-Fi adapter cannot host on %s GHz; Windows will choose the band.", BandName(band));
      band = Operators::TetheringWiFiBand::Auto;
    }
  }

  // Windows turns an idle hotspot off after a few minutes; keep it up while
  // the headset is not on yet, and restore the user's preference afterwards.
  bool restoreTimeout = false;
  try {
    if (Operators::NetworkOperatorTetheringManager::IsNoConnectionsTimeoutEnabled()) {
      Operators::NetworkOperatorTetheringManager::DisableNoConnectionsTimeout();
      restoreTimeout = true;
    }
  } catch (const hresult_error&) {}  // older Windows: the watchdog below restarts it

  // Prefer a session-only configuration (Windows 11 24H2+): it never touches
  // the hotspot name/password saved in Settings. Otherwise configure the
  // persistent one and put the previous values back when stopping.
  std::optional<Operators::NetworkOperatorTetheringAccessPointConfiguration> previous;
  auto start = [&](Operators::TetheringWiFiBand useBand) -> Operators::NetworkOperatorTetheringOperationResult {
    if (auto session = manager.try_as<Operators::INetworkOperatorTetheringManager2>()) {
      try {
        Operators::NetworkOperatorTetheringSessionAccessPointConfiguration config;
        config.Ssid(ssid);
        config.Passphrase(password);
        config.Band(useBand);
        config.AuthenticationKind(Operators::TetheringWiFiAuthenticationKind::Wpa2);
        // The headset's video outranks this PC's own traffic to the router.
        try { config.PerformancePriority(Operators::TetheringWiFiPerformancePriority::TetheringOverStation); }
        catch (const hresult_error&) {}
        return session.StartTetheringAsync(config).get();
      } catch (const hresult_error& error) {
        if (error.code() != REGDB_E_CLASSNOTREG && error.code() != E_NOINTERFACE) throw;
      }
    }
    if (!previous) {
      Operators::NetworkOperatorTetheringAccessPointConfiguration saved;
      saved.Ssid(current.Ssid());
      saved.Passphrase(current.Passphrase());
      try { saved.Band(current.Band()); } catch (const hresult_error&) {}
      previous = saved;
    }
    Operators::NetworkOperatorTetheringAccessPointConfiguration config;
    config.Ssid(ssid);
    config.Passphrase(password);
    try { config.Band(useBand); } catch (const hresult_error&) {}
    manager.ConfigureAccessPointAsync(config).get();
    return manager.StartTetheringAsync().get();
  };
  // Global user settings first (instant), then the slower radio stop.
  auto cleanup = [&] {
    if (restoreTimeout) {
      try { Operators::NetworkOperatorTetheringManager::EnableNoConnectionsTimeout(); } catch (const hresult_error&) {}
      restoreTimeout = false;
    }
    try {
      if (manager.TetheringOperationalState() != Operators::TetheringOperationalState::Off)
        manager.StopTetheringAsync().get();
    } catch (const hresult_error&) {}
    if (previous) {
      try { manager.ConfigureAccessPointAsync(*previous).get(); } catch (const hresult_error&) {}
      previous.reset();
    }
  };

  try {
    auto result = start(band);
    if (result.Status() != Operators::TetheringOperationStatus::Success &&
        band != Operators::TetheringWiFiBand::Auto && !stopping) {
      Out("DIRECT_WIFI_NOTE Could not start on %s GHz (%s); retrying with automatic band.",
          BandName(band), StatusName(result.Status()));
      band = Operators::TetheringWiFiBand::Auto;
      result = start(band);
    }
    if (result.Status() != Operators::TetheringOperationStatus::Success) {
      const auto extra = to_string(result.AdditionalErrorMessage());
      Out("DIRECT_WIFI_NOTE Mobile Hotspot did not start: %s%s%s", StatusName(result.Status()),
          extra.empty() ? "" : " - ", extra.c_str());
      cleanup();
      return Outcome::Unavailable;
    }
    Out("DIRECT_WIFI_MODE=hotspot");
    Out("DIRECT_WIFI_BAND=%s", BandName(band));
    Out("DIRECT_WIFI_INTERNET=%s %s", source->internet ? "shared" : "local", sourceName.c_str());
    ReportStation();
    Out("DIRECT_WIFI_STARTED");
    started = true;
    Out("DIRECT_WIFI_RADIO streaming-mode=%s", radio.Engage() ? "on" : "unavailable");
    ReportAddress(10000);

    uint32_t clients = UINT32_MAX;
    int restarts = 0;
    while (Wait(1000)) {
      const auto state = manager.TetheringOperationalState();
      if (state == Operators::TetheringOperationalState::Off) {
        // Idle timeout on older Windows, or a radio reset. Bring it back.
        if (++restarts > 5) { Fail("Mobile Hotspot keeps turning off"); cleanup(); return Outcome::Failed; }
        Out("DIRECT_WIFI_NOTE Windows turned the hotspot off; turning it back on.");
        if (start(band).Status() == Operators::TetheringOperationStatus::Success) ReportAddress(10000);
        continue;
      }
      if (state != Operators::TetheringOperationalState::On) continue;
      const uint32_t count = manager.ClientCount();
      if (count != clients) {
        Out("DIRECT_WIFI_CLIENTS=%u", clients = count);
        if (count) restarts = 0;
      }
    }
    radio.Release();
    cleanup();
    Out("DIRECT_WIFI_STOPPED");
    return Outcome::Stopped;
  } catch (const hresult_error& error) {
    cleanup();
    if (started) { Fail("0x%08X: %ls", unsigned(error.code()), error.message().c_str()); return Outcome::Failed; }
    Out("DIRECT_WIFI_NOTE Mobile Hotspot failed: %ls", error.message().c_str());
    return Outcome::Unavailable;
  }
}

// ---- Wi-Fi Direct legacy AP (local only) --------------------------------------------
struct PeerState {
  std::mutex mutex;
  std::vector<WiFiDirectDevice> devices;
  bool closing = false;
};
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
      printf("PEER_CONNECTED\nDIRECT_WIFI_CLIENTS=%zu\n", state->devices.size());
      for (const auto& endpoint : device.GetConnectionEndpointPairs()) {
        if (endpoint.LocalHostName().Type() != Windows::Networking::HostNameType::Ipv4) continue;
        printf("DIRECT_WIFI_ADDRESS=%s\n", to_string(endpoint.LocalHostName().CanonicalName()).c_str());
        break;
      }
      fflush(stdout);
    } else device.Close();
  } catch (const hresult_error& error) {
    fprintf(stderr, "Peer connection failed: %ls\n", error.message().c_str());
  }
}

Outcome RunLegacy(const std::wstring& ssid, std::wstring& password) {
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
  auto connectionSubscription = listener.ConnectionRequested(auto_revoke, [peers](auto const&, WiFiDirectConnectionRequestedEventArgs const& args) {
    AcceptPeer(args.GetConnectionRequest(), peers);
  });
  auto statusSubscription = publisher.StatusChanged(auto_revoke, [](auto const&, WiFiDirectAdvertisementPublisherStatusChangedEventArgs const& args) {
    switch (args.Status()) {
      case WiFiDirectAdvertisementPublisherStatus::Started:
        started = true; break;
      case WiFiDirectAdvertisementPublisherStatus::Aborted:
        failed = true; Fail("error=%d", int(args.Error())); break;
      case WiFiDirectAdvertisementPublisherStatus::Stopped:
        if (!stopping && !failed) { failed = true; Fail("Windows stopped the Wi-Fi Direct network"); }
        break;
      default: break;
    }
  });
  if (stopping) return Outcome::Stopped;
  publisher.Start();
  const auto deadline = GetTickCount64() + 15000;
  while (!stopping && !failed && !started) {
    if (GetTickCount64() >= deadline) { Fail("startup timed out"); failed = true; break; }
    Sleep(100);
  }
  if (started && !failed) {
    Out("DIRECT_WIFI_MODE=legacy");
    Out("DIRECT_WIFI_INTERNET=none");
    ReportStation();
    Out("DIRECT_WIFI_STARTED");
    Out("DIRECT_WIFI_RADIO streaming-mode=%s", radio.Engage() ? "on" : "unavailable");
    ReportAddress(10000);
  }
  while (started && !stopping && !failed) Sleep(100);
  radio.Release();
  publisher.Stop();
  {
    std::lock_guard<std::mutex> guard(peers->mutex);
    peers->closing = true;
    for (auto& peer : peers->devices) peer.Close();
    peers->devices.clear();
  }
  if (!failed) Out("DIRECT_WIFI_STOPPED");
  return failed ? Outcome::Failed : Outcome::Stopped;
}

// Read-only: lists adapters and whether Mobile Hotspot could start. Never
// touches the radio.
int Probe() {
  HANDLE client = nullptr;
  DWORD version = 0;
  if (WlanOpenHandle(2, nullptr, &version, &client) != ERROR_SUCCESS) { puts("NO_WIFI_ADAPTER"); return 1; }
  PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
  const auto result = WlanEnumInterfaces(client, nullptr, &interfaces);
  if (result == ERROR_SUCCESS) for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i)
    wprintf(L"WIFI_ADAPTER %ls\n", interfaces->InterfaceInfo[i].strInterfaceDescription);
  const bool found = result == ERROR_SUCCESS && interfaces->dwNumberOfItems > 0;
  if (interfaces) WlanFreeMemory(interfaces);
  WlanCloseHandle(client, nullptr);
  if (!found) { puts("NO_WIFI_ADAPTER"); return 1; }
  puts("DIRECT_WIFI_REQUIRES_START_CHECK");
  try {
    init_apartment(apartment_type::multi_threaded);
    std::string reason;
    if (auto source = PickProfile(reason)) {
      printf("DIRECT_WIFI_HOTSPOT_AVAILABLE internet=%s source=%s\n", source->internet ? "shared" : "local",
             to_string(source->profile.ProfileName()).c_str());
      const auto manager = Operators::NetworkOperatorTetheringManager::CreateFromConnectionProfile(source->profile);
      if (manager.TetheringOperationalState() == Operators::TetheringOperationalState::On) puts("DIRECT_WIFI_HOTSPOT_ON");
      try {
        if (manager.GetCurrentAccessPointConfiguration().IsBandSupported(Operators::TetheringWiFiBand::FiveGigahertz))
          puts("DIRECT_WIFI_BAND_SUPPORTED 5");
      } catch (const hresult_error&) {}
    } else {
      printf("DIRECT_WIFI_HOTSPOT_UNAVAILABLE %s\n", reason.c_str());
    }
  } catch (const hresult_error& error) {
    printf("DIRECT_WIFI_HOTSPOT_UNAVAILABLE %ls\n", error.message().c_str());
  }
  if (const ULONG mhz = StationMhz()) printf("DIRECT_WIFI_STATION_MHZ=%lu\n", mhz);
  fflush(stdout);
  return 0;
}
}  // namespace

namespace wvd {
int RoleNetwork(int argc, wchar_t** argv) {
  std::wstring ssid, password, stopEvent, mode = L"auto", band = L"5";
  bool probe = false;
  for (int i = 1; i < argc; ++i) {
    const std::wstring argument = argv[i];
    if (argument == L"--probe") probe = true;
    else if (argument == L"--ssid" && i + 1 < argc) ssid = argv[++i];
    else if (argument == L"--stop-event" && i + 1 < argc) stopEvent = argv[++i];
    else if (argument == L"--mode" && i + 1 < argc) mode = argv[++i];
    else if (argument == L"--band" && i + 1 < argc) band = argv[++i];
    else return 2;
  }
  if (probe) return Probe();  // never activates or changes the radio
  if (mode != L"auto" && mode != L"hotspot" && mode != L"legacy") return 2;
  if (band != L"auto" && band != L"2.4" && band != L"5" && band != L"6") return 2;
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
  struct Wipe { std::wstring& s; ~Wipe() { SecureZeroMemory(s.data(), s.size() * sizeof(wchar_t)); s.clear(); } } wipe{password};
  try {
    init_apartment(apartment_type::multi_threaded);
    if (stopping) return 0;
    if (mode != L"legacy") {
      const auto outcome = RunHotspot(ssid, password, ParseBand(band));
      if (outcome != Outcome::Unavailable || stopping) return outcome == Outcome::Failed ? 1 : 0;
      if (mode == L"hotspot") { Fail("Mobile Hotspot could not start; see the note above"); return 1; }
      Out("DIRECT_WIFI_NOTE Falling back to a local-only Wi-Fi Direct network (no internet for the headset).");
    }
    return RunLegacy(ssid, password) == Outcome::Failed ? 1 : 0;
  } catch (const hresult_error& error) {
    Fail("0x%08X: %ls", unsigned(error.code()), error.message().c_str());
    return 1;
  }
}
}  // namespace wvd

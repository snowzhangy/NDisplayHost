// ndisplay — the Windows host command line: one executable, one role per process.
//
//   ndisplay stream  [options]            capture, encode, QUIC transport
//   ndisplay input   [options]            console input helper (starts disarmed)
//   ndisplay owner   [options]            software device, topology, service
//   ndisplay network [options]            offline direct Wi-Fi access point
//   ndisplay diag    <command> [options]  bring-up and measurement probes
//   ndisplay --version                    host name and version
//
// The control panel (NDisplayHostWin.exe) starts these roles as its children;
// NDisplayHostWin.exe with a role argument forwards to the same code. Running a
// role as its own process is what keeps a crashing media worker from taking the
// panel, the input helper or the virtual monitor down with it (§4.1).
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "wvd/roles.h"
#include "wvd/version.h"

namespace {

constexpr const char* kUsage =
    "NDisplayHostWin \u2014 Windows display host for Apple Vision Pro\n"
    "usage: ndisplay <role> [options]\n"
    "\n"
    "  stream    capture, encode and stream over QUIC on UDP 47800\n"
    "            (--help lists --monitor, --fps, --bitrate, --codec, --input \u2026)\n"
    "  input     keyboard/mouse helper for this session; starts disarmed\n"
    "  owner     software device, topology save/arrange/restore, keep-awake;\n"
    "            --install | --uninstall | --start | --stop need elevation\n"
    "  network   offline direct Wi-Fi access point (password on stdin)\n"
    "  diag      adapters | caps | modes | topology | capture | quic-probe |\n"
    "            owner-ping | coords-selftest | curtain-test\n"
    "\n"
    "  --version  print the host name and version\n"
    "\n"
    "Each role is a separate process; one role never holds another's resources.\n";

std::string Utf8(const wchar_t* text) {
  if (!text) return {};
  const int needed =
      WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 0) return {};
  std::string out(static_cast<size_t>(needed) - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
  return out;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) {
    std::fputs(kUsage, stderr);
    return 2;
  }
  const std::string role = Utf8(argv[1]);
  if (role == "-h" || role == "--help" || role == "help") {
    std::fputs(kUsage, stdout);
    return 0;
  }
  if (role == "-V" || role == "--version" || role == "version") {
    // Every log and screenshot can then say which host build produced it.
    std::printf("%s %s\n", wvd::kHostProduct, wvd::HostVersion().c_str());
    return 0;
  }

  // The narrow roles keep their existing argument parsing; hand them the same
  // command line in UTF-8, with argv[0] naming the role for their usage text.
  std::vector<std::string> owned;
  owned.reserve(static_cast<size_t>(argc));
  owned.push_back("ndisplay " + role);
  for (int i = 2; i < argc; ++i) owned.push_back(Utf8(argv[i]));
  std::vector<char*> narrow(owned.size(), nullptr);
  for (size_t i = 0; i < owned.size(); ++i) narrow[i] = owned[i].data();
  const int narrowArgc = static_cast<int>(narrow.size());

  if (role == "stream") return wvd::RoleStream(narrowArgc, narrow.data());
  if (role == "input") return wvd::RoleInput(narrowArgc, narrow.data());
  if (role == "owner") return wvd::RoleOwner(narrowArgc, narrow.data());
  if (role == "network") return wvd::RoleNetwork(argc - 1, argv + 1);  // wide: SSID, password
  if (role == "diag") return wvd::RoleDiag(narrowArgc, narrow.data());

  std::fprintf(stderr, "unknown role '%s'\n", role.c_str());
  std::fputs(kUsage, stderr);
  return 2;
}

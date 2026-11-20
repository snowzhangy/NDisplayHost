// The roles compiled into the single command-line host, ndisplay.exe.
//
// One executable, one process per role: the dispatcher in src/cli picks a role
// from the first argument and calls it with the rest of the command line. Each
// role keeps the process boundaries the design asks for (§4.1) — the stream
// worker, the input helper, the device-owner service and the direct-Wi-Fi
// access point still run as separate processes, they are just the same binary.
#pragma once

namespace wvd {

// Capture, encode and serve WIRE v1 over QUIC (was wvd-media.exe).
int RoleStream(int argc, char** argv);
// Console keyboard/mouse injection helper, starts disarmed (was wvd-input.exe).
int RoleInput(int argc, char** argv);
// Software device, topology and keep-awake; also installs/runs as a service
// (was wvd-owner.exe).
int RoleOwner(int argc, char** argv);
// Optional offline Wi-Fi Direct access point (was wvd-network.exe). argv is
// wide because the SSID and the password from stdin are not limited to ASCII.
int RoleNetwork(int argc, wchar_t** argv);
// Bring-up and measurement probes (was wvd-diag.exe).
int RoleDiag(int argc, char** argv);

}  // namespace wvd

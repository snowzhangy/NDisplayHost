# Connections, pairing and offline use

The control panel starts the Windows development TCP host. The Windows media worker
can also use [QUIC and authenticated pairing](README.md#quic-transport-wire-v1) from
the command line when built with MsQuic.

## Existing network

PC and headset need a local route to each other. Ethernet on the PC and Wi-Fi
on the headset can share the same LAN. Internet access is not required. Guest,
hotel and airplane Wi-Fi may isolate clients, preventing this connection.
The present local diagnostic receiver uses TCP port 50001 by default.

## Direct Wi-Fi fallback

The PC can create its own local access point using the public Windows Wi-Fi
Direct legacy-AP API. A client joins it through ordinary Wi-Fi settings; it does
not need access to Apple's private networking frameworks or a Wi-Fi Direct API.
This is a router-free local connection, not a cellular/internet tether.

1. Expand **Direct Wi-Fi · no router** in `wvd-host`.
2. Set an SSID (1–32 UTF-8 bytes) and a password (8–63 printable ASCII characters).
   The panel generates a random password on launch and masks it; **Copy** is
   available for the operator. The password is never put in logs or command lines.
3. Click **Create direct Wi-Fi**. Wait for Windows to report it started. Merely
   opening the panel or probing adapters does not activate the radio.
4. On the headset, select this SSID in Settings → Wi-Fi and enter the password.
   Internet unavailability is expected for this local-only network.
5. Stop direct Wi-Fi with the same button or close the panel.

The helper is isolated from the UI and keeps the access-point publisher and
client connections alive. Graceful stop closes connections and stops publishing;
the UI job also contains the helper if the UI crashes. No Wi-Fi profile,
registry setting, firewall rule or internet-sharing configuration is installed.
Windows chooses the radio/channel; a preferred band is not exposed in this build.

On the development laptop, `netsh wlan show wirelesscapabilities` reports the
MediaTek MT7925 supports Wi-Fi Direct GO and GO on 5 GHz. That is capability
evidence, not proof of successful access-point creation with a headset.
Wi-Fi must be on, the driver must support this API, and Windows Mobile Hotspot
must be off. Startup failures are shown in the panel and Activity log.
On an aircraft, radio use also depends on the operator's permitted configuration.

Implementation references: Microsoft's
[desktop legacy-AP sample](https://github.com/microsoft/Windows-classic-samples/tree/main/Samples/WiFiDirectLegacyAP)
and [Wi-Fi Direct desktop guidance](https://learn.microsoft.com/en-us/windows-hardware/drivers/partnerapps/wi-fi-direct).

## Handshake once Windows implements the shared protocol

The existing Apple client discovers a host through `_wvd._udp`, opens QUIC/TLS
and exchanges capabilities. New-device pairing requires an explicitly opened
pairing window: both devices show the same six-digit authentication string and
the user confirms both. The headset pins the host certificate; the host stores
the client's public key. Reconnect authenticates the remembered identity.
Keyboard/mouse consent is a separate decision; video does not depend on granting it.
See [`core/proto/WIRE.md` §7](../core/proto/WIRE.md#7-pairing-and-authentication) for the exact contract.

## What the original Mac Virtual Display reference establishes

The supplied `MVD_disassembly_core` is static analysis, not a packet capture.
It shows Rapport discovery/control names such as `_rdlink._tcp`, AVConference
RTP/RTCP connection initialization and SRTP/SRTCP settings, plus an explicit UDP
audio path and AWDL/infrastructure selection. A `_tcp` discovery name does not
establish that every video packet uses TCP; nor does the audio evidence prove
the transport for every video configuration. This Windows implementation uses
its own documented protocol rather than Apple's private Sidecar stack.

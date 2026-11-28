# Connections, pairing and offline use

The control panel and media worker serve [WIRE v1 over QUIC](README.md#quic-transport-wire-v1)
on UDP port 47800.

## Existing network

PC and headset need a local route to each other. Ethernet on the PC and Wi-Fi
on the headset can share the same LAN. Internet access is not required. Guest,
hotel and airplane Wi-Fi may isolate clients, preventing this connection.
On a trusted local network, choose the Private network profile in Windows.
On first Start, the host offers to install a Windows Firewall rule for
`ndisplay.exe` and UDP 47800. **Allow through firewall** retries the setup.
The rule allows connections from the local subnet on Private and Domain
profiles. Bonjour discovery also needs Windows and the LAN to pass multicast
DNS (UDP 5353). If discovery is unavailable, add the receiver address shown
in the host panel.

## Direct connect · Vision Pro (no router)

Turn on **Direct connect · Vision Pro** and this PC becomes the Wi-Fi network
the headset joins. Video goes straight from the PC's radio to the headset, with
no router hop and no other devices sharing the airtime.

1. Switch on **Direct connect · Vision Pro**. That is the only step on the PC:
   the host starts too, and the first time only, one firewall question is asked
   (it covers the LAN and direct connect; see below).
2. On Vision Pro, open Settings → Wi-Fi and join the network shown on the card.
   The password is displayed in large plain text while the network waits for
   the headset, so it can be read through passthrough. Generated passwords are
   lowercase groups such as `kq7m-29xd-pf4h`, which are easy to type.
3. NDisplay finds this PC by itself: an already paired PC simply shows as
   online under its usual name. On the first connection,
   confirm the same pairing code on both devices.

Direct connect stays on: if it was on when the panel closed, it turns back on
(and starts the host) the next time the panel opens. Day to day, open the
panel and put on the headset; Vision Pro rejoins the known network and
reconnects. Switching it off returns the headset to its usual Wi-Fi. The name,
password and band are under **Network settings** and stored for this Windows
user (password DPAPI-encrypted) in `%LOCALAPPDATA%\WVM\direct-connect.txt`.

### How the headset keeps internet access

The helper (`ndisplay network`) tries two Windows mechanisms, in this order:

| Backend | When it is used | Headset internet | Band |
| --- | --- | --- | --- |
| **Windows Mobile Hotspot** (`NetworkOperatorTetheringManager`) | the PC has any connected network: Ethernet, or Wi-Fi to a router | **Yes**: Windows shares the PC's uplink through Internet Connection Sharing (NAT plus DHCP on 192.168.137.0/24) | 5 GHz by default; automatic or 2.4 GHz selectable |
| **Wi-Fi Direct legacy AP** | no network to share (e.g. offline or on a plane), or Mobile Hotspot is blocked by policy | No: local-only | chosen by Windows |

On Windows 11 24H2 and later, the hotspot is started with a *session*
configuration, so the Mobile Hotspot name and password saved in Windows
Settings are left untouched. On older builds those settings are replaced while
direct connect is on and put back when it stops. If Mobile Hotspot was already
on, direct connect uses it as configured and leaves it on afterwards. Windows'
"turn off when no devices are connected" timer is suspended while direct
connect is on and restored afterwards.

### Radio and performance notes

* Best: the PC on **Ethernet**. The Wi-Fi radio then serves only the headset.
* PC on Wi-Fi to a router: most adapters, including the MT7925, have one
  channel. The hotspot shares the router link's channel and airtime. The
  session asks Windows to favour hotspot traffic (`TetheringOverStation`).
  If the PC's router link is 2.4 GHz, the headset network is forced to
  2.4 GHz too, and the panel warns about it.
* While direct connect is on, the helper turns on WLAN *media streaming mode*
  and turns off *background scanning* on the PC's Wi-Fi card
  (`WlanSetInterface`, no elevation). A background scan takes the shared
  radio off the headset's channel. Measured on an MT7925 before this change:
  a 1–2 s loss burst (10–30% loss, RTT flat) every 12–15 s. Both settings
  are restored when direct connect stops, and Windows also reverts them if
  the helper exits. Side effect while on: the PC's own Wi-Fi does not roam
  or refresh its list of nearby networks.
* `netsh wlan show wirelesscapabilities` lists "Wi-Fi Direct GO" and "P2P GO on
  5 GHz" support for an adapter.

### Firewall

Windows treats the PC's own access point as an unidentified, **Public**
network, which the LAN rule (Private/Domain, LocalSubnet) does not cover.
**Allow through firewall** (or the prompt shown when direct connect is first
switched on) adds a second rule, `NDisplayHostWin direct connect UDP 47800`:
inbound UDP 47800 to `ndisplay.exe`, Wi-Fi interfaces only, from the ICS scope
(`192.168.137.0/24`, or the administrator's `ScopeAddress`) only. The built-in
*mDNS (UDP-In)* rule already covers Public networks, so Bonjour works.

### Discovery

The stream role advertises `_wvd._udp` under the PC's name on the best LAN
interface and, with the **same name**, on the Mobile Hotspot / Wi-Fi Direct
interface whenever one is up. A headset is only ever on one of the two
networks. NDisplay reconnects to the Bonjour name it last used, so a headset
moved from the router to direct connect mid-session finds the PC again by
itself. If Windows rejects the second registration of the name, the hotspot
advertisement falls back to `<PC name> (Direct)`; NDisplay matches either
one to the paired PC by its TXT `id`.

Implementation references: [NetworkOperatorTetheringManager](https://learn.microsoft.com/uwp/api/windows.networking.networkoperators.networkoperatortetheringmanager),
Microsoft's [desktop legacy-AP sample](https://github.com/microsoft/Windows-classic-samples/tree/main/Samples/WiFiDirectLegacyAP)
and [Wi-Fi Direct desktop guidance](https://learn.microsoft.com/en-us/windows-hardware/drivers/partnerapps/wi-fi-direct).

## Pairing

The existing Apple client discovers a host through `_wvd._udp`, opens QUIC/TLS
and exchanges capabilities. Both devices display a six-digit code on the first
connection. Compare the codes and confirm on both devices. The headset pins the host certificate; the host stores
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

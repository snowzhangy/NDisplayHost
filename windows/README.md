# NDisplayHostWin — Windows virtual monitor host for Apple Vision Pro

The user-facing name of this host is **NDisplayHostWin** followed by its version,
for example `NDisplayHostWin 1.0.0 (build 1)`. It appears in the control-panel title bar,
the header, the tray tooltip and the first line of every `wvd-media` log, so a
screenshot or a log is enough to say which build a machine is running. Bump
`WVM_HOST_VERSION_MAJOR/MINOR/PATCH` and `WVM_HOST_BUILD` in
[`CMakeLists.txt`](CMakeLists.txt) — the single source of truth; the C# panel
(`src/ui/Version.cs.in`) and the native binaries (`src/common/include/wvd/
version.h`) are generated from it. The executable and component names
(`wvd-host.exe`, `wvd-media.exe`, …), the `WVM_HOST_*` CMake variables, the
`WVM_HOME` override with its `%LOCALAPPDATA%\WVM` store, the `wvm-pairing-*`
pipes, the `_wvd._udp` service type and the driver's `WVD` virtual-monitor
identifiers are unchanged: they are internal, and renaming them would break
existing pairings, saved hosts and installed drivers for no user benefit.

Windows host for desktop capture, video encoding, audio, cursor updates and remote input.
The user-mode components build with MSVC and the Windows SDK. The IddCx driver
requires a separate WDK build and installation.

The host serves [WIRE v1](../core/proto/WIRE.md) over QUIC on UDP port **47800**.
In [NDisplayApp](https://github.com/snowzhangy/NDisplayApp), select the discovered
PC or add its LAN address using **Secure QUIC**. Compare the six-digit code on
the PC and headset when pairing.

| Component | Role |
| --- | --- |
| `wvd-owner` | Software-device lifetime, topology recovery and keep-awake |
| `wvd-media` | Desktop Duplication → BT.709 full-range NV12 → encoder → transport |
| `wvd-input` | Consented, per-session keyboard/mouse injection |
| `wvd-diag` | Adapter/encoder probes, capture diagnostics and QUIC listener probe |
| `wvd-testcard` | Animated window for capture and latency testing |
| `wvd-host` | **NDisplayHostWin** control panel for streaming, input consent and connection setup |
| `wvd-network` | Optional offline Wi-Fi Direct access point, owned by the control panel |
| `driver/` | UMDF/IddCx virtual monitor provider, separate WDK project |

See the [wire contract](../core/proto/WIRE.md) and
[validation record](VALIDATION.md) for implementation scope and test evidence.

## Build and checks

Use an x64 Visual Studio developer shell with MSVC, the Windows 10/11 SDK,
CMake ≥ 3.24 and Ninja. From `windows/`:

```powershell
scripts\build-msquic.ps1
scripts\build.ps1
ctest --test-dir build --output-on-failure
```

Or configure manually:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

Tests cover input state and pipe lifecycle, GPU colour conversion, cursor
shapes, EDID encoding, and topology persistence/remapping with simulated CCD
devices. The pipe integration test requires Python 3; the GPU test needs a D3D11
hardware adapter. Lock/UAC transitions and driver loading require manual checks.

### AMD development laptop

Keep the machine's optimized Release build in **windows/build**. From an x64
Visual Studio developer shell in **windows/**:

```powershell
cmake --preset amd
cmake --build --preset amd
ctest --preset amd
```

The AMD preset disables NVIDIA SDK encoding and retains AMD hardware
HEVC/H.264 through Media Foundation with GPU texture input. It uses the same
build folder as the desktop launcher. Alternate NVIDIA or tray build folders
are unnecessary on this machine.

### Optional NVENC

Point CMake at NVIDIA's Video Codec SDK, containing `Interface/nvEncodeAPI.h`
(the `include/` layout is accepted too):

```powershell
cmake -S . -B build -G Ninja -DWVD_ENABLE_NVENC=ON -DWVD_NVENC_SDK_DIR=C:/nvenc/Video_Codec_SDK
cmake --build build
```

The module loads `nvEncodeAPI64.dll` from System32, checks the API version,
opens the encode session on the capture device when that is an NVIDIA adapter,
registers the converted NV12 textures and encodes them in place: synchronous
HEVC Main with no B-frames or lookahead, keyframes on demand, bitrate changed
through `nvEncReconfigureEncoder`. VPS/SPS/PPS are published before video and
repeated on recovery keyframes. On a hybrid laptop whose display hangs off the
integrated GPU, NVENC is skipped and that GPU's own encoder is used, which
avoids a cross-adapter copy. Compilation with NVIDIA API 13.1 headers is
verified; NVIDIA runtime operation is not verified on the AMD development
machine. `-DWVD_ENABLE_NVENC=OFF` disables the module. Without the SDK, NVIDIA
GPUs are driven through NVIDIA's hardware MFT (`mf-hw`).

## QUIC transport (WIRE v1)

Streams carry the control plane, unreliable datagrams carry picture shards, FEC and NACKs recover
loss, and pairing is derived from the TLS exporter secrets so the six-digit code the operator reads
binds the headset to this host and to nobody else.

The required MsQuic build uses the pinned revision and Schannel TLS. See
[Build and checks](#build-and-checks) to compile it.

```powershell
wvd-media --listen 47800
```

The host publishes `_wvd._udp`. Allow inbound UDP 47800 on the Windows network
profile used by the headset. The application also accepts a different listener
port via `--listen PORT`.

| Switch | Effect |
| --- | --- |
| *(default)* | New devices may pair; the code must match on host and headset |
| `--pair-auto` | Pair without the code check — trusted network only, and the log says so |
| `--no-pair` | New devices are refused; paired headsets still connect |
| `--allow-input` with `--input-token` | Allows a paired headset to drive mouse and keyboard through the local helper |

The control panel prompts when a headset pairs. For a command-line session,
read the code in `PAIR <request id> <code> <device name>` and send one message
to the local pairing pipe printed by the worker.

```powershell
# Substitute the worker PID from the pairing-pipe log and the matching request ID.
$pipe = [System.IO.Pipes.NamedPipeClientStream]::new('.', 'wvm-pairing-1234', 'Out')
try {
  $pipe.Connect(1500)
  $decision = [Text.Encoding]::ASCII.GetBytes('1 accept')
  $pipe.Write($decision, 0, $decision.Length)
} finally {
  $pipe.Dispose()
}
```

Use `deny` to reject the request.

Pairings and the host identity live in
`%LOCALAPPDATA%\WVM` (`paired-devices.txt`, `host-cert.der`); `WVM_HOME` relocates both, which is how
the regression test keeps its pairings out of the real host's list. `ctest -R quic-host-session`
drives a scripted headset through pairing and the code comparison, flow binding, layout and codec
negotiation, media datagrams with FEC rebuild and NACK recovery, ping, input translation, a stale
session being refused, and disconnect.

## Run for testing

Open the desktop control panel from the repository root:

```powershell
windows/build/src/ui/wvd-host.exe
```

Choose the display, encoder, frame rate and bitrate, then click **Start host**.
The default **Primary display (automatic)** follows the current primary display
when a session connects. **Refresh** reloads the active display list, and Start
also refreshes it. Explicit choices use the monitor's device interface when
available, so Windows renumbering DISPLAY33 to DISPLAY49 does not lose the
selected monitor. If that monitor is removed, select an available display;
an explicit selection never silently switches to another screen.
**Test listener** checks the local UDP socket, QUIC handshake and datagram negotiation.
The test can run during an active streaming session. Input
control is off by default. Enable it before starting only for a trusted client;
**Disable input** immediately disarms the helper. **Minimize** hides the panel
in the Windows notification area (system tray) while streaming keeps running.
Click the NDisplayHostWin monitor icon to reopen it, or right-click for
**Open NDisplayHostWin** and **Exit NDisplayHostWin**; the tooltip names the running version. The icon
may be under the notification area's overflow arrow.
Closing the panel or choosing **Exit NDisplayHostWin** stops all
processes and any direct Wi-Fi network it created. Existing independently
started command-line processes are not managed by this panel.

**Test card** opens an optional animated pattern with a frame counter. It helps
spot freezes and capture delay; it is not the control panel or a video receiver.

The panel uses WPF and the .NET Framework compiler included on the development
PC; CMake builds it without NuGet packages. If that compiler is unavailable,
CMake reports that the UI target is skipped and the native tools still build.

### No router or internet: direct Wi-Fi fallback

Expand **Direct Wi-Fi · no router**, choose a network name/password, and click
**Create direct Wi-Fi**. Join that network from the headset's Wi-Fi settings.
It is a local access point without internet sharing; an internet uplink is not
required. The radio is unchanged until the button is clicked. Stop it with the
same button or close the panel. See [connection details](CONNECTIONS.md).

The laptop's MediaTek MT7925 driver reports Wi-Fi Direct group-owner and 5 GHz
group-owner support. The helper compiles and its argument/stop paths are tested;
actual radio startup and a headset joining still need manual validation. Windows
Mobile Hotspot conflicts with this mode and must be off.

### Command-line launcher

From the repository root, in your interactive Windows session:

```powershell
powershell -ExecutionPolicy Bypass -File windows/scripts/start-dev.ps1 -TestCard
```

This starts the input helper, the host on UDP **47800**, and the test card.
Logs and process IDs are in `.local/dev-session/`. Pass `-Probe` to test the QUIC
handshake or `-NoInput` to omit the input helper. `-ScreenOff` blacks out the PC's
screens while a headset is connected. `-NoAudio` keeps the PC's sound off the stream.

```powershell
# From windows/
build/src/media/wvd-media.exe --listen 47800 --codec auto
build/src/tools/diag/wvd-diag.exe quic-probe 47800

# Run capture and encoding locally with synthetic input.
build/src/media/wvd-media.exe --synthetic --demo 5 --codec auto
```

`--monitor \\.\DISPLAY2` selects an output; `--fps 60` sets the stream frame
rate and synthetic cadence. Desktop Duplication emits desktop changes rather
than a fixed number of frames per second; capture is paced so the stream never
exceeds `--fps`, whatever the display refreshes at (a 120–180 Hz panel would
otherwise multiply bitrate and GPU load). Capture polls without blocking the
shared GPU device, on a high-resolution timer. A frame that only moved the
pointer sends a cursor update and is not copied or encoded.

### Audio to Vision Pro

Audio needs this updated Windows host as well as the headset client. Earlier host builds ignore the
headset's request and send no sound. On the headset, turn on **Play computer audio** (display menu →
Sound). The host captures the default playback device with WASAPI shared-mode loopback and sends
16-bit stereo PCM at the device's mix rate in 10 ms packets; the PC speakers keep playing.

- **On and off without reconnecting.** The headset asks for sound in its hello and sends the hello again
  whenever that changes: sound switched on or off, or every display window hidden or shown. The host
  starts or stops capturing on each one, so a hidden display costs no audio bandwidth. (The first audio
  host build also starts on a repeated hello, but cannot stop until the headset disconnects.)
- **Follows the default playback device.** Picking other speakers, or plugging in or removing
  headphones, switches the capture to the new default device within about a second. A PC without a
  playback device is retried every second.
- **Surround mixes keep their dialogue.** A 5.1 or 7.1 device is folded to stereo: front left/right at full
  level, centre into both sides and surround/back channels into their side at -3 dB, LFE left out. Louder
  surround mixes are clipped, not wrapped.
- **Switch it off on the PC:** clear **Send this PC's sound to the headset** in the control panel, or start
  `wvd-media --no-audio`. The host then refuses the headset's request and logs it once.

The host log shows `audio=1` in each peer hello that asks for sound, then `audio: loopback '<device>'`
with the format (and `folded to stereo` for more than two channels). `audio: off (headset)` follows a hello
without it. `audio: loopback capture unavailable` reports the Windows error once per outage. The
headset's Sound panel shows whether audio is arriving. Windows host build artifacts include Media
Foundation encoders. The optional NVIDIA Video Codec SDK and the separate virtual display driver
installation are described in the build instructions above.

### PC screen off while streaming

Like a Mac running Mac Virtual Display, the PC's own screens can go dark while the headset is connected:
tick **Turn off this PC's screen while the headset is connected** in the control panel, or start
`wvd-media --screen-off`. Each physical screen gets a black, click-through, topmost window that is
**excluded from capture** (`SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`, Windows 10 2004 or later),
so Desktop Duplication and the headset still see the desktop beneath it. For the first ten seconds it
shows a dim line explaining how to get the screen back.

- The screens come back when the headset disconnects, and **Ctrl+Alt+Shift+B** shows them (press it again
  to blank them) for the rest of the session. If the media worker exits for any reason its windows close
  with it, so nothing has to be restored.
- The displays stay on and in the Windows topology: capture, window positions, the size menu and input
  are unchanged, and a size change from the headset re-fits the curtain. Turning a captured panel off for
  real would stop desktop composition and the stream with it. WVD virtual monitors and other indirect
  displays are never covered, since no one looks at them.
- If Windows refuses capture exclusion, the streamed screen is left visible (logged) instead of
  streaming black; other screens are still covered.
- Not covered: the secure desktop (UAC prompts, the lock screen), Start and other shell flyouts that sit
  above topmost windows, and the hardware pointer. An LCD's backlight stays on; an OLED panel shows true
  black. A full-screen game under the curtain is composed by DWM rather than flipped directly.

`wvd-diag curtain-test` checks this on a PC (unlocked desktop required): it shows a small magenta
rectangle near the top-left corner for about two seconds and verifies, through Desktop Duplication, that
the curtain hides it on screen but not in the captured picture. It also lists which screens
`--screen-off` would cover.

### Pipeline, picture size and link adaptation

The picture stays on the GPU: the desktop surface is copied to a ring texture,
converted to an NV12 texture, and handed to the hardware encoder as a texture
(NVENC registered resource, or a D3D11-aware Media Foundation encoder sharing
the capture device). Only the software and test encoders, or a hardware MFT
that declines texture input, read the picture back to the CPU. The stats line
shows `gpu-texture` or `cpu-copy`.

| Option | Default | Effect |
| --- | --- | --- |
| `--bitrate KBPS` | 60000 | Ceiling. The target drops while the link is behind and climbs back afterwards |
| `--min-bitrate KBPS` | ceiling / 8 | Floor for that adaptation |
| `--fixed-bitrate` | off | Keep the target at `--bitrate` |
| `--max-pixels N` | 8294400 | Largest encoded picture (3840×2160). A bigger desktop is scaled to fit, keeping its aspect ratio; `0` encodes at desktop size |
| `--gop-sec N` | 0 | Periodic keyframe every N seconds. `0` = on demand only (first picture, recovery request, encoder switch) |
| `--synthetic-size WxH` | 1280x720 | Size of the `--synthetic` test source |

The pixel budget follows Apple's Mac Virtual Display, whose headset passes a
3840×2160 pixel budget into the encoder-size computation even for a 10240×2880
desktop. On the AMD development PC a 3840×2160 picture encodes in about 9 ms
of the 16.7 ms frame; 5120×2880 at desktop size takes about 29 ms and does not
hold 60 fps.

When encoded pictures stop leaving (the only congestion signal a reliable
stream gives), the encoder is held back before it encodes: the newest desktop
picture waits and older ones are skipped. Every encoded picture is still
delivered in order, so the reference chain stays intact and no keyframe is spent
on a link that is already behind. If the peer stalls for longer than 250 ms the
bounded send queue can still overflow; the next encode is then an IDR.

The target bitrate drops by a quarter only for a link that **keeps** falling
behind: at least three separate waits adding up to 250 ms within one second,
while a picture is waiting (at most one cut per 500 ms). Short Wi-Fi pauses and
keyframe bursts drain on their own and only skip pictures. The target climbs
back by an eighth of the ceiling per second, starting 1.5 s after the last
backlog, also while the desktop is still. The log names the evidence, e.g.
`bitrate 60000 -> 45000 kbps (link behind: pictures waited 279 ms in 1 s)`.
Before 2026-11-16 a single wait of one frame time cut the target and it climbed
back only after 4 quiet seconds, so Wi-Fi hiccups ratcheted a 60 Mbit/s stream
down to 20-30 Mbit/s.

For a game that pauses while the FPS counter stays high, inspect the Activity
log's `gaps[cap/send]` (worst interval between captures / completed picture
sends) and `send-p95` (time spent writing a picture). These reset each reporting
window. A large capture gap points upstream of the network; a large send time
indicates sending is blocked. The host's encode p95 helps distinguish a busy
encoder from a busy link. A still desktop legitimately has long frame gaps.
Start comparisons at 40 Mbps and 60 fps, then change one setting at a time.
A higher bitrate ceiling uses more bandwidth; it does not increase frame rate.
The headset's FPS is an average of completed GPU texture writes, not a measure
of evenly spaced updates or the headset compositor's refresh rate. Its frame
gap p95 / worst row covers the last 120 update intervals.

While a headset is connected the media worker holds a display-and-system power
request. Without it the display idles off, Windows stops composing the desktop,
and the stream freezes on its last picture.

### Display size from the headset

The monitor window's control bar on the headset has a size menu, like Mac
Virtual Display's Standard / Wide / Ultrawide. Apple's headset sends the Mac a
resolution request and the Mac re-applies its virtual display's modes; here the
media worker lists the sizes the streamed monitor reports (`DisplayModes`,
kind 18) and applies the one picked (`ModeChange`, kind 4) with a dynamic
`ChangeDisplaySettingsEx`. Desktop Duplication then reports ACCESS_LOST, and the
normal resize path rebuilds the ring, encoder and layout, so the headset gets a
new `MonitorLayout` and `CodecConfig` and its window takes the new shape.

* The change lasts for the session. When the headset disconnects the monitor
  goes back to its configured mode. The headset remembers the last size per
  host and asks for it again on the next connection.
* The WVD virtual monitor offers 16:9 (up to 3840×2160), 21:9 (2560×1080,
  3440×1440, 5120×2160) and 32:9 (3840×1080, 5120×1440). Sizes over the encode
  budget are marked "streamed scaled" in the menu.
* A physical monitor offers what its driver lists, at its current refresh rate
  when that size supports it. Sizes below 1280×720 are left out, and a rotated
  monitor offers only its current mode.
* `--no-mode-change` advertises only the current mode, so the menu is hidden.
  Requests come from the authenticated headset session.

`wvd-diag modes` prints the list each display would offer.

### Encoder selection

`--codec auto` selects NVENC when built/usable, then a hardware Media Foundation
encoder (`mf-hw`, including AMD/Intel), then Microsoft's software H.264 encoder.
Native AMF and D3D12 encoding are still probes/stubs. `auto` never selects the
null encoder. `hevc` and `h264` select codec preferences; the actual codec is
always reported in `CodecConfig`.

Pin a specific encoder with `--codec mf-hw`, `--codec mf-sw-h264`, or
`--codec nvenc-hevc`. `--codec null`/`null-test` emits test data that a real
decoder cannot display. An unavailable pinned encoder fails initialization;
runtime encoder errors can still fall back to another tier.

Video access units and parameter sets use Annex B start codes. A Vision Pro
receiver must split NAL units, build its VideoToolbox format description, and
convert access units to the length-prefixed form expected by VideoToolbox.

## Input consent and lifecycle

`wvd-input` runs inside the interactive session, and starts disarmed. For a
manual launch, copy its printed token into the media worker:

```powershell
# Terminal 1; elevate for injection into elevated applications:
build/src/input/wvd-input.exe

# Terminal 2; substitute the token printed above:
build/src/media/wvd-media.exe --listen 47800 --allow-input --input \\.\pipe\wvd.input --input-token TOKEN
```

The option is **`--input`**, not `--input-pipe`. Without a token, video works
and input forwarding stays off. The launcher supplies a fresh token itself.
Remote input also requires the operator to enable it and the headset to authenticate.

The hook-free injector thread owns desktop attachment and `SendInput`. A
separate thread handles desktop/WTS notifications. Switching desktops flushes
queued events and releases held input; session lock suspends injection while
preserving consent. Unlock resumes it. Logoff/disconnect, disarm, queue overflow,
or the armed pipe connection closing revokes consent and releases input.
Generations belong to the helper and stale/future batches are rejected. A
restarted helper has a new instance ID so the worker can distinguish it from
explicit revocation. Failed release operations retain held state for retry.

An ordinary elevated administrator cannot necessarily access the secure UAC or
Winlogon desktop. Full secure-desktop testing needs a suitably privileged helper
launched **inside the console session**, not a Session-0 process. Touch/pen
injection and some uncommon HID keys remain unimplemented.

## Owner and topology

Install the [driver package](driver/README.md) before using the owner to create
a virtual monitor. Save/arrange/restore from an elevated interactive terminal:

```powershell
build/src/owner/wvd-owner.exe --save-topology
build/src/owner/wvd-owner.exe --arrange WVD
build/src/owner/wvd-owner.exe --restore-topology
```

Version-2 snapshots store active monitor device identities, source modes, clone
groups, rotation, scaling and refresh rate. Restore resolves current routes and
adapter IDs, validates the configuration, then applies it. Missing monitors
cause restore to fail without applying a partial topology. **Re-save old
version-1 snapshots**; their persisted LUIDs cannot safely be migrated.

The LocalSystem service delegates CCD work to a contained, fixed-command child
in the active console session. Its watchdog restores missing saved monitors,
without undoing a user's ordinary position changes. This service/session bridge
still needs an installed-service test. Service stop is reported complete only
after IPC callbacks, watchdog work, power requests and the software device stop.

## Diagnostics

```text
wvd-diag adapters        DXGI adapters and outputs
wvd-diag caps            encoder candidates
wvd-diag modes           sizes the headset's size menu would offer per display
wvd-diag topology        active display paths (--all for every path)
wvd-diag capture         Desktop Duplication probe (--seconds N)
wvd-diag coords-selftest coordinate transforms
wvd-diag curtain-test    --screen-off curtain stays out of the stream
wvd-diag owner-ping      owner control pipe
wvd-diag quic-probe PORT local QUIC handshake and datagram negotiation
```

Monochrome cursor height excludes the second mask. Colour alpha is retained;
masked-colour alpha selects replace/XOR, so black replacement pixels remain
opaque. Destination-dependent XOR/inversion uses a grey overlay approximation.

The driver remains unbuilt without the WDK, and Vision Pro decoding/input has
not been tested with a headset in this workspace. This is a development host,
not a signed driver installer or a finished headset application.

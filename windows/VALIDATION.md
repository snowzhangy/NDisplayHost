# Windows validation — 2026-11-15

## UDP-only Windows host

The host uses QUIC over UDP for discovery and sessions. The loopback suite exercises
the WIRE v1 handshake, code comparison, flow binding, peer datagram limits, FEC,
retransmission, codec reconfiguration, audio, cursor, input and shutdown. The launcher
test also reserves the same TCP port and verifies the real worker answers over UDP.

```powershell
ctest --test-dir windows/build --output-on-failure
```

To sample software transport under load, run
`windows/build/tests/wvd-quic-session-tests.exe --measure`. It feeds 60 Mbit/s of
synthetic access units at 60 fps into WIRE v1, then reports loopback payload
delivery, peak send backlog and local drops. This measures packetization and
transport on the PC. Wi-Fi, headset decoding and presentation require a device run.

The entries below are historical results for earlier host builds.


Validated on the AMD development PC with MSVC 19.51 and Windows SDK 10.0.26100.
These checks cover the current development transport and user-mode host.

* Default MSVC/Ninja build: passed.
* SDK-enabled NVENC build using NVIDIA API 13.1 headers: passed. The public
  header is downloaded into the ignored `windows/third_party/nvenc/` directory;
  it is not vendored or committed. NVIDIA hardware/runtime encoding is untested.
* WPF control panel: compiled with the installed .NET Framework compiler. A
  window-driven smoke test initialized choices, started synthetic streaming to
  the local receiver, received live metrics, disarmed input, and gracefully
  stopped all owned host/input/receiver processes. The rendered panel was
  checked for layout. No keyboard/mouse input was injected.
* Offline Wi-Fi helper: MSVC/C++20 build, read-only WLAN adapter probe, invalid
  credentials/SSID rejection, and pre-signalled launcher shutdown passed without
  enabling the radio. The MediaTek MT7925 driver reports Wi-Fi Direct GO and
  5 GHz GO support. Access-point startup and Vision Pro association are untested;
  the network stays off until the operator clicks Create direct Wi-Fi.
* CTest: input state/held-release retry, HID translation, cursor masks/alpha,
  EDID fields/checksum, GPU BT.709 colour samples, simulated topology reboot
  remapping/route conflicts/query races/corrupt snapshots, and real input-helper
  IPC authentication/generation/connection lifecycle passed. The pipe test
  sends zero input events and skips acceptance when the session is suspended.
  A slow in-memory receiver also verifies late dependent pictures are delivered
  and send-queue overflow recovers with an IDR before further dependent data.
* Real Desktop Duplication probe outside the sandbox: 2560×1600 output captured
  successfully. The sandbox denies DDA access with E_ACCESSDENIED.
* Real desktop → AMD hardware HEVC → local wire receiver: codec configuration
  precedes video; VPS/SPS/PPS and Annex B data validate; requested recovery IDR
  contains its parameter sets. Source changes were delivered continuously.
* The live test exposed shared-device blocking during AcquireNextFrame. After
  polling without a DXGI wait, conversion measured approximately 2.6 ms median
  and 3.1 ms p95, compared with approximately 90 ms before the change. Desktop
  frame counts reflect changes on screen, not a sustained 60 fps benchmark.
* Running launcher session with the animated test card: received 187 HEVC
  pictures at 2560?1600 during the five-second wire check; input helper armed
  on connect and released/disarmed on disconnect. No input events were injected.
* GPU conversion now preserves red/blue channel order and BT.709 full-range
  values. Encoded dependent frames are sent in order; an overflow forces the
  next encode to IDR rather than continuing a broken reference chain.

## Pipeline update — 2026-11-15 (AMD development PC, Radeon 8060S, 2560×1600 @ 180 Hz panel)

Measured over the TCP wire with a local receiver, old build against new build.
"CPU" is `wvd-media` process time as a share of one core.

Correction (same day): until the `FrameRing::WriteCpuFrame` fix below, every
picture of the synthetic source was black (see "Found and fixed"). The
synthetic rows marked † were measured on black pictures in both builds; the
relative comparison holds (the readback and copy costs do not depend on the
picture), the absolute encoder numbers do not. The new-build list below was
re-measured with real content after the fix.

| Case | Before | After |
| --- | --- | --- |
| Synthetic 1280×720, 60 fps † | 60.0 fps, 3 keyframes in 6 s, CPU 18.8% | 60.0 fps, 1 keyframe, CPU 10.9% |
| NV12 conversion stage, 1280×720 † | 0.88 ms median (GPU + readback + CPU unpack) | 0.17 ms (GPU pass submitted, no readback) |
| NV12 conversion stage, real desktop 2560×1600 | 2.04 ms median | 0.14 ms |
| Encoder input, AMD HEVC MFT | CPU buffer | D3D11 texture (zero-copy) |
| Link throttled to 25 Mbit/s, 1280×720, 12 s † | 283 of 692 encoded pictures delivered, 404 dropped after encoding | 643 of 643 delivered, 0 dropped, target settled near 23 Mbit/s (real content: 645 of 645, 55 skipped before encode, target 20.8 Mbit/s) |

New-build only (the old build has no size option), real synthetic content,
AMD HEVC MFT with texture input. CPU here is dominated by the synthetic
generator drawing and uploading the pattern on the CPU, not by the pipeline:

* Synthetic 1280×720: 60.0 fps, 60 Mbit/s, 1 keyframe in 6 s.
* Synthetic 2560×1600: 60.0 fps, encode 5.4 ms median / 6.7 ms p95.
* Synthetic 3840×2160: 60.2 fps, encode 9.1 ms median / 10.5 ms p95.
* Synthetic 5120×2880 with the default pixel budget: encoded at 3840×2160,
  59.7 fps, encode 9.6 ms median.
  With `--max-pixels 0` (desktop size): encode 29.4 ms median, 30.6 fps
  (a second run reached 58.4 fps; it does not hold 60).
* Dynamic bitrate change is honoured by the AMD MFT (60 → 45 → 33.75 → 25.3 →
  19 Mbit/s steps logged, output rate followed).
* Software H.264 (CPU readback path): 60.0 fps, 7.2 Mbit/s, 45 KB keyframe
  at 1280×720. The null encoder runs; synthetic wire smoke test (setup order,
  Annex B, recovery IDR) passed; CTest 6/6 passed.
* Real desktop → AMD HEVC with texture input ran for 152 pictures without
  error before the PC locked.

Not measured in this pass:

* Frame pacing against an animated desktop on the 180 Hz panel, pointer-only
  frame skipping and the session power request under real use. The PC locked
  during the run, and Desktop Duplication is denied on the secure desktop.
  Re-run the real-desktop commands below with the test card
  (`wvd-testcard --fps 120`) and expect a steady 60 fps.
* NVENC texture input, on-demand keyframes and `nvEncReconfigureEncoder` are
  compiled against the 13.1 headers only; no NVIDIA GPU was available.
* Intel and NVIDIA hardware MFTs with texture input. If one declines it, the
  encoder falls back to CPU buffers and logs the reason.

Found and fixed: `FrameRing::WriteCpuFrame` passed a `D3D11_BOX` with
`back = 0`, which is an empty box, so `UpdateSubresource` never wrote anything
and the slot stayed black. It is used by the synthetic source and by the GDI
bootstrap frame, so a headset that connected to a still desktop got a black
first picture until something on screen changed. This is also why the software
H.264 encoder appeared to emit only ~80 kbit/s: it was encoding black frames.
`wvd-gpu-tests` now runs a 1280×720 picture through ring → NV12 texture →
readback and checks every luma and chroma sample, plus a scaled conversion.

### Display size from the headset — 2026-11-15

Checked on the AMD development PC:

* `wvd-diag modes` on the 2560×1600 @ 180 Hz panel lists 10 sizes from
  2560×1600 (current, native) down to 1280×720, each at 180 Hz.
* Wire round trip against `wvd-media --synthetic` with a Python peer: the list
  follows `MonitorLayout` with the same layout generation; a request for the
  current size answers "unchanged"; a request for another size answers
  "unsupported" (the synthetic source has a fixed size).
* `apple_tcp_fixture.py` sends a list and answers a request the way the host
  does, checked with a Python client; the synthetic wire smoke test and CTest
  6/6 still pass.

Not checked:

* Applying a real mode change. It would resize the live desktop of this PC, and
  the PC was locked during this pass (Desktop Duplication denied), so the
  apply → ACCESS_LOST → resize → new layout path and the restore at session
  end have not run.
* The Swift side (envelope messages, Windows TCP mapping, `AppModel`, size
  menu) and the new `WindowsTCPWireTests` / `WindowsTCPTests` cases have not
  been compiled: this PC has no Swift toolchain. Run `swift test` in
  `mac/WVDKit` and build the visionOS app on the Mac.
* The wide and ultrawide IddCx modes, which need the WDK build of the driver.

Reproduce from the repository root in a developer shell:

```powershell
cmake --build windows/build
ctest --test-dir windows/build --output-on-failure
python windows/tests/stream_smoke_test.py windows/build/src/media/wvd-media.exe --synthetic
python windows/tests/stream_smoke_test.py windows/build/src/media/wvd-media.exe
```

The last command requires desktop access outside restricted execution contexts.
It receives video in memory and does not save desktop pictures.

### Gaming stutter diagnostics — 2026-11-15

Corrected the Media Foundation rate-control buffer request. Microsoft's
[H.264](https://learn.microsoft.com/en-us/windows/win32/medfound/h-264-video-encoder)
and [HEVC](https://learn.microsoft.com/en-us/windows/win32/medfound/h-265---hevc-video-encoder)
encoder contracts specify `CODECAPI_AVEncCommonBufferSize` in bytes. The old
code requested a frame's **bits**, eight times the intended buffer. Initial
configuration and dynamic bitrate updates now request a frame's bytes using
the rational frame rate. This remains a best-effort property: an MFT can
decline it. It is a real unit bug, not proof of the reported game stall's cause.

Added per-reporting-window capture/send worst gaps and picture-send p95 to
the host log and control panel. The Vision Pro stats add p95/worst intervals
over the last 120 successful GPU texture writes. Average FPS can conceal an
isolated long gap followed by a burst; the new Swift regression includes
a 200 ms gap in a sequence averaging about 50.6 fps. Static desktops can
legitimately have long gaps. Windows TCP stats also mark unavailable encoder
timing and TCP loss information instead of displaying misleading zeroes.

Checked on the AMD Radeon 8060S PC:

* Default build, all six CTest suites, and SDK-enabled NVENC compile passed.
  Regression checks cover byte sizing (including fractional rates and range
  limits), slow-send telemetry and ordered dependent-frame delivery.
* WPF smoke test passed: device choices, live telemetry, input disarm and
  graceful process cleanup.
* Synthetic 1280×720, AMD hardware HEVC, 80 Mbps target: 275 pictures;
  arrival spacing p50 15.87 ms, p95 18.59 ms, worst 32.50 ms.
* Real 2560×1600 desktop with the 120 Hz moving test card, AMD hardware HEVC,
  80 Mbps target: 276 pictures, steady reporting windows around 60 fps;
  arrival spacing p50 15.71 ms, p95 17.70 ms, worst 31.46 ms. No encoded
  pictures dropped or skipped for link congestion. Capture did not change
  the monitor mode. Video was received in memory without saving pictures.
* Real still-desktop smoke also passed setup/Annex B/recovery checks. As
  expected it produced only a few pictures per second; this is not a motion
  performance test.

These were short localhost tests, not a game, Wi-Fi or headset performance
measurement. The reported intermittent game stall still needs an in-game
comparison with the host gap/encode measurements and network details. The
Swift additions have not been compiled here: no Mac/Xcode/Swift toolchain is
available on this Windows PC. Run `swift test` in `mac/WVDKit`, build the
visionOS app and validate on the headset before distributing this revision.

### Minimize to the notification area — 2026-11-15

The control panel now owns a monitor-shaped notification icon with **Open WVD**
and **Exit WVD** actions. Minimizing hides the panel and its taskbar button;
clicking the icon restores the prior normal/maximized state. Exit and the
window close button share the existing graceful media/input/network shutdown.
The notification icon is removed when that shutdown finishes.

The UI built successfully, and its smoke test passed with a separate synthetic
stream: telemetry continued while the window was hidden, the panel restored,
input disarmed and the child processes stopped. The previous live panel was
then closed gracefully and the updated host restarted at 60 fps / 60 Mbps.

The read-only adapter/mode check still finds only the physical 2560×1600 panel;
the IddCx source's 4K/wide/ultrawide modes are not installed on this PC. No
monitor modes or driver-signing settings were changed for this UI update.

Audio companion validation (2026-11-15):

* [Windows CI run](https://github.com/snowzhangy/WVM/actions/runs/43800191240) compiled the full user-mode
  host with MSVC and the Windows SDK. `core-regressions` and `topology-regressions` passed, and the run
  produced the `WVM-Windows-Audio-Host` executable package. The optional NVENC SDK was not included.
* The Apple client passed 40 isolated client tests, including Windows TCP PCM parsing/resampling and
  regressions that block audio conversion while ingress and session replacement stay responsive.
* These checks do not validate WASAPI capture from a physical playback device, audible headset output,
  or sustained game/video performance. Those still require the Windows PC and Vision Pro test.

### Audio follow-up and PC screen off — 2026-11-16

Merged main (Vision Pro audio, curved displays, saved profiles, the Windows loopback sender) into the
display-size work. `DisplayModes` moved from packet kind 17 to 18, because main's `Audio` took 17.

Audio changes on the host: each hello now starts *or stops* audio, so the headset re-sends its hello
when sound is switched or all displays hide or show (the Swift side does this on `StreamSettings.audio`).
The capture follows the default playback device and retries when none is available. It polls on the
high-resolution timer (a `Sleep(5)` woke every 15.6 ms). 5.1/7.1 devices are folded to stereo; before,
only the first two channels were sent, which loses the centre (dialogue) and surrounds.
`--no-audio` and a control-panel switch refuse the headset's request.

PC screen off (`--screen-off`, control-panel switch, `start-dev.ps1 -ScreenOff`): a black,
click-through, topmost window per physical screen, excluded from capture. See the README section.

Checked on the AMD Radeon 8060S PC:

* Build, all six CTest suites. `core-regressions` adds the stereo fold-down (stereo bit-exact, mono,
  5.1 centre and back-left at -3 dB, LFE dropped, default 7.1 order without a mask, an unassigned
  channel, clamping, NaN) and which monitors the curtain may cover (WVD's `#WVD` device path,
  indirect displays, an internal panel).
* Live WASAPI loopback (synthetic video, a Python stand-in headset, PC locked): device
  `Speakers (Realtek(R) Audio)`, 48 kHz, 2 ch, float. Digital silence and then a -72 dBFS 1 kHz tone were
  rendered on the default device (inaudible); the host sent 10 ms packets (480 frames) in real time with
  no sequence gaps, and the tone arrived at amplitude 7.7 of 8. A hello without the audio bit stopped
  all audio packets; the next hello with it resumed them. Nothing was recorded or saved.
* `apple_tcp_fixture.py` driven by a Python client: initial order layout, display modes (18), codec…;
  hellos 0/1/0 give no audio, then one malformed + 50 valid packets, then nothing; the size request is
  answered on kind 18.
* Control panel: snapshot shows the two new switches; its smoke test passed (synthetic stream,
  tray, input disarm, cleanup).
* `wvd-diag curtain-test` with the PC locked: the panel is listed as covered, the curtain window is
  created, Windows accepted `WDA_EXCLUDEFROMCAPTURE` for it and Ctrl+Alt+Shift+B registered. The capture
  half of the test (the stream sees through the curtain) needs an unlocked desktop and was **not run**.

Not checked: the curtain against real Desktop Duplication and on the headset, its effect on capture
timing, multi-monitor and mixed-DPI layouts, a default-device switch or a 5.1/7.1 device on real
hardware, and the new Swift test `windowsAudioSwitchesOnAndOffMidSession` (no Swift toolchain here).

### Bitrate control on Wi-Fi — 2026-11-16

First headset session on the new host: the encoder took about 5 ms and almost every
picture left at once (send p95 0.07 ms), yet the target was cut 12 times (60 down to
19 Mbit/s) and restored twice. A single wait longer than one frame time cut it by a
quarter; it climbed back only after 4 quiet seconds, by 10% per second. Short Wi-Fi
pauses therefore ratcheted it down. `LinkRate` (session/link_rate.h) now cuts only for a
sustained backlog (3+ waits adding up to 250 ms in 1 s, while a picture is waiting)
and climbs back an eighth of the ceiling per second after 1.5 s.

Checked: `core-regressions` replays timelines (Wi-Fi pauses plus keyframe bursts, a
single 250 ms outage, a link that cannot keep up, recovery, a still desktop, the
floor). Loopback A/B with a 2560x1600 synthetic desktop, AMD hardware HEVC, 60 Mbit/s
ceiling and a 64 KB receive window:

| Receiver | Old rule | New rule |
| --- | --- | --- |
| 150 ms pause every 1.5 s | 3 cuts, ends near 30 Mbit/s, 31.4 Mbit/s received | no cut, 60 Mbit/s, 56.2 Mbit/s received |
| 80 ms pause every 1.5 s | no cut, 58.4 Mbit/s | no cut, 58.4 Mbit/s |
| 15 Mbit/s reader | 6 cuts, ends at 12.9 Mbit/s | 6 cuts, ends at 16.3 Mbit/s |

Not yet checked on the headset's Wi-Fi.

### Stale display selection and reconnect loops — 2026-11-16

The supplied 21:06 log showed successful headset connections followed by
"no matching output for '\\.\DISPLAY33'" on every attempt. Current DXGI
enumeration exposed DISPLAY49. The failure was display selection before
capture began; qWAVE's optional WMM warning did not abort the connection.

The panel now defaults to Automatic primary display, refreshes its display
choices before Start, and has an explicit Refresh action. Fixed selections
use the monitor device interface from EnumDisplayDevicesW when available.
The media worker resolves an explicit GDI name before listening and retains
the interface across sessions. On capture loss it re-resolves that identity;
if its GDI name or GPU changed, it ends the old session so the next connection
can rebuild capture/encoding on the current output. Missing explicit targets
never fall back to another monitor. Removed the misleading blanket session-0
hint and unsafe substring matching of DISPLAY33 against DISPLAY333.

Validation:

* Windows build and all six CTest suites passed. Output selection regressions
  cover GDI renumbering with a stable identity, Automatic primary selection,
  a missing monitor and exact matching against similarly numbered displays.
* Passing --monitor \\.\DISPLAY33 failed preflight with an actionable selection
  error rather than accepting and closing headset connections.
* Real capture through the monitor interface succeeded on DISPLAY49:
  hardware HEVC, 69 pictures, codec setup order, Annex B and recovery IDR
  checks passed. No desktop pictures were saved.
* WPF smoke passed display refresh/identity preservation, Automatic selection,
  live telemetry, minimize/restore, input disarm and process cleanup.
* No forced hotplug/renumbering or GPU migration was performed on the live
  desktop. These recovery paths still need a hardware/headset retest.

### Single AMD Release build and connection UI — 2026-11-17

Synced the current Windows branch with main. Closed both old control-panel
instances gracefully, removed the generated build, build-nvenc and build-tray
directories, and recreated only windows/build using the amd CMake preset.
The cache confirms Release with /O2 /Ob2, NVIDIA SDK encoding disabled, and
tests enabled. Source files and session logs were retained.

The obsolete "Vision Pro pairing not available" card now gives the supported
Windows TCP connection steps. Ready/listening text points to the headset, and
the Local test tooltip explains that its diagnostic receiver occupies the
single connection.

The clean build and all six CTest suites passed. Synthetic hardware HEVC
encoded through AMDh265Encoder with GPU texture input at about 60 fps; 228
pictures passed codec setup order, Annex B and recovery IDR checks. The UI
smoke test passed display refresh, tray/restore, telemetry and cleanup. A
render of the panel's own visual confirmed the updated connection card.
No headset or in-game performance measurement was made in this pass.

Still needs manual/hardware validation:

* UAC/secure-desktop switches, lock/unlock, logoff and real held-key release
  during a media-worker crash, using the correctly privileged session helper.
* Installed LocalSystem owner service, console-session delegation, shutdown,
  and physical display restore across reboot/hotplug/clone/rotation cases.
* WDK compilation, INF/package validation, signing, driver installation and
  monitor arrival/drain/removal under IddCx.
* NVIDIA GPU encode/recovery/resource teardown; actual headset VideoToolbox
  decoding, cursor rendering and input round trip. The Apple client now has an explicit Windows TCP
  compatibility mode, exercised on macOS with an independent Python wire peer and real synthetic
  HEVC/H.264 decoding. Physical PC/headset interoperability still needs the manual test described in
  [`core/proto/WINDOWS_TCP.md` in Git history. The QUIC and pairing contract is in
  [`core/proto/WIRE.md`](../core/proto/WIRE.md).

XOR cursor compositing is approximated on the overlay plane. QUIC, production
pairing/consent UI, touch/pen injection, and HDR are outside this patch.

## Named "WVM Host" with a visible version — 2026-11-17 (AMD development PC)

The Windows host's user-facing name is now **WVM Host** followed by its version,
`WVM Host 1.0.0 (build 1)`. It is defined once, in `WVM_HOST_NAME`,
`WVM_HOST_VERSION_MAJOR/MINOR/PATCH` and `WVM_HOST_BUILD` in `CMakeLists.txt`, and
flows to:

* the control panel through the generated `src/ui/Version.cs`
  (`Version.cs.in`): window title, panel header, activity-log window title,
  tray tooltip and the **Open/Exit WVM Host** menu items;
* the native workers through `src/common/include/wvd/version.h`, which logs
  `WVM Host 1.0.0 (build 1)` as the first line of every `wvd-media` log;
* CI, which names the uploaded artifact
  `WVM-Host-<version>-build<build>` and repeats it in the package README.

The direct Wi-Fi network default became `WVM-<machine>`/`WVM-Direct`. Process,
target and library names (`wvd-host.exe`, `wvd-media.exe`, `wvd-*` CMake
targets), the wire protocol and the driver's `WVD` virtual-monitor identifiers
(used by the screen curtain and its tests) are deliberately unchanged.

Verified on this machine: clean `cmake --build build` (MSVC, all targets
including the panel), all six CTest suites passed, `wvd-media --demo 2` logged
`WVM Host 1.0.0 (build 1)` as its first line, and the running panel's window
title read back as `WVM Host 1.0.0 (build 1)`. The UI smoke test passed display
refresh/identity selection, live telemetry, minimize-to-tray, restore, input
disarm and cleanup; its first attempt in this pass reported "No live stream
telemetry" within the 3.5 s window and passed on an immediate rerun, so that
timeout remains a known flake unrelated to this change.

## QUIC (WIRE v1) host transport — 2026-11-19 (AMD development PC)

`wvd-media --quic` speaks [WIRE v1](../core/proto/WIRE.md) over QUIC alongside the framed TCP
development transport: control and input on streams, picture shards on unreliable datagrams, XOR FEC
plus NACK recovery, cursor and audio on the control stream, and pairing whose six-digit code is
derived from the TLS exporter secrets (`wdauth.cpp`). msquic is an optional dependency found in
`windows/third_party/msquic/build/{bin,obj}/Release`; without it the tree builds TCP-only and
`--quic` says so. One streaming session per host, input stays disarmed until `--allow-input`, and
paired devices live in `%LOCALAPPDATA%\WVM\paired-devices.txt` (`WVM_HOME` relocates them, which is
what keeps the test's pairings out of the real host's list).

Verified on this machine: clean `cmake --build build` with msquic present, and `ctest` passing 8/8
including the new `quic-host-session`, which runs a scripted headset through pairing and the
host/headset code comparison, session flow binding, layout and codec negotiation, media datagrams
with FEC rebuild and NACK recovery, ping, packed-input translation, a stale session being refused
and disconnect. Six consecutive runs of that test passed with no flakes. The real binary was started
by hand and logged `quic: listening on udp:47800 (ALPN wvd/1, idle 15000 ms)`, the pairing pipe, and
`DNS-SD: registered …_wvd._tcp.local.` / `…_wvd._udp.local.`.

Four host-side bugs were found by that test and fixed, all of them capable of breaking a real
headset rather than only the test: a stream send handed msquic a descriptor (and the payload a
stack frame) that were dead by the time the flush ran, so the first frame was right and later traffic
was arbitrary memory; `StreamReceiveComplete` was called after a synchronous consume, which stalls
stream delivery after the first receive — the same mistake for datagrams put a freed descriptor on
the wire and crashed inside the msquic flush; the connection handle was closed by the callback while
the client still held it, so destroying a client after a disconnect faulted; and the control handlers
locked the session mutex recursively (`std::mutex` throws, and the throw escaped a msquic callback).
`BCryptImportKeyPair` for device verification was also importing under an unfinalised key handle,
which made every pairing signature fail to verify.

Not validated: a real Vision Pro joining a QUIC session on this LAN (no device here), datagrams
larger than the 1200-byte QUIC minimum (the packetiser is sized from the headset's Hello, but the
negotiated path MTU was never exercised), sustained throughput and congestion behaviour, and the
panel's pairing prompt — the code currently reaches the media log and `\.\pipe\wvm-pairing`, and the
operator answers over that pipe.

## Renamed to "NDisplayHostWin" — 2026-11-19 (AMD development PC)

The Vision Pro client family is now **NDisplay**, so the Windows host's
user-facing name became **NDisplayHostWin** (the macOS host keeps its own name).
Only the product string changed: `WVM_HOST_NAME` in `CMakeLists.txt` now reads
`NDisplayHostWin`, which flows to the panel's window title, header, activity-log
title, tray tooltip and **Open/Exit NDisplayHostWin** menu items through the
generated `src/ui/Version.cs`; `wvd/version.h`, the certificate subject
(`identity.Load(...)`) and the direct-Wi-Fi SSID default
(`NDisplay-<machine>` / `NDisplay-Direct`) were updated to match. CI names the
artifact `NDisplayHostWin-<version>-build<build>`.

Deliberately unchanged, because they are internal and renaming them would break
existing pairings, saved hosts and installed drivers: the `WVM_HOST_*` CMake
variables, `WVM_HOME` and its `%LOCALAPPDATA%\WVM` identity/pairing store, the
`WVM Host Identity` certificate key container, the `wvm-pairing-<pid>` pipe, the
`_wvd._udp` service type, the `wvd-*` executable and target names, and the
driver's `WVD` virtual-monitor identifiers.

Verified on this machine: full `cmake --build build` (MSVC, all targets including
the panel), `ctest` 8/8 passed, and the running panel's window title read back as
`NDisplayHostWin 1.0.0 (build 1)`.

## Two executables: NDisplayHostWin.exe and ndisplay.exe — 2026-11-19 (AMD development PC)

The host used to ship seven executables (`wvd-host`, `wvd-media`, `wvd-input`,
`wvd-owner`, `wvd-network`, `wvd-diag`, `wvd-testcard`) in a mirrored
`src/<component>/` tree. It now ships two:

* **`NDisplayHostWin.exe`** — the WPF control panel, the only GUI;
* **`ndisplay.exe`** — the command-line host, with the other components as
  *roles* selected by the first argument: `stream`, `input`, `owner`, `network`,
  `diag`, plus `--version`.

Each role is still its own process. The process split is what §4.1 asks for — a
crashing media worker must not take the panel, the armed input helper or the
virtual monitor down with it, and the owner still runs as a LocalSystem service
while the input helper stays inside the interactive session — so the roles were
merged into one *binary*, not into one *process*. `wvd-diag`'s commands became
`ndisplay diag <command>`; the animated **test card** and the development-only
`wvd-quic-spike` were removed. The role entry points are declared once in
`src/common/include/wvd/roles.h`, each former `main()` became `wvd::Role*` in
its own file, and `src/cli/main.cpp` dispatches with wide arguments converted to
UTF-8 so a non-ASCII SSID or password survives.

Both executables are now written to `build/bin` beside `msquic.dll`, which is
also the release layout: the panel finds the CLI next to itself instead of
reaching into `../media`. A role's log starts with
`NDisplayHostWin 1.0.0 (build 2)` (`wvd/version.h`, used for the first time), and
`ndisplay.exe --version` prints the same line. Pairings, pipes
(`wvm-pairing-<pid>`, `wvd.input`), the identity store, `_wvd._udp` and the
service name `WvdOwner` are unchanged, so existing headsets keep pairing.

Verified on this machine: `cmake --build --preset amd` clean from an empty build
directory; `ctest --preset amd` 8/8, with `input-ipc` and
`launcher-stop-and-network-validation` now driving `ndisplay input|stream|network|diag`
from one binary; `ndisplay stream --synthetic --demo` encodes through the AMD
hardware HEVC encoder; a UI Automation pass opened the panel, confirmed the
**Test card** button is gone, clicked **Start host**, saw exactly one
`ndisplay.exe` child appear, clicked **Stop host** and confirmed the child
exited; `ndisplay.exe` with no arguments prints the role list and exits 2.

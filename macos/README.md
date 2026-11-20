# macOS host

The **WVD Host** menu bar app streams a selected display or a synthetic test pattern via QUIC. The sender uses ScreenCaptureKit, Metal, and VideoToolbox. macOS prompts for Screen Recording permission for real-display capture and Accessibility permission before injecting input. System audio (48 kHz stereo, with the host's own sound excluded) is sent as PCM datagrams only while the headset asks for it in `StreamSettings.audio`; the synthetic source sends a test tone.

Use macOS 26 or later with Xcode and Swift 6.2 or later. From this directory:

```sh
open WVD.xcodeproj
xcodebuild -project WVD.xcodeproj -scheme WVDHost -destination 'platform=macOS' CODE_SIGNING_ALLOWED=NO build
swift test --package-path HostKit
```

Select your development team in Xcode for a signed app. `xcodegen generate` can regenerate the checked-in project from `project.yml`.

The CLI is also available from the same package:

```sh
swift run --package-path HostKit -c release wvd-host serve
swift run --package-path HostKit -c release wvd-host serve --display 0 --allow-input
swift run --package-path HostKit -c release wvd-host bench
swift run --package-path HostKit -c release wvd-host bench --width 1920 --height 1080 --mbps 10 --quality --h264
```

`bench` encodes 120 synthetic frames and prints per-stage timings. `--mbps` sets the target bitrate, `--h264`
selects H.264, `--quality` reports luma PSNR against the encoder's NV12 input and `--low-latency-rc` switches
VideoToolbox to low-latency rate control.

First connection: choose **Pair New Device…** in the menu, pick the Mac on the headset, compare the six-digit
code on both screens and confirm on both. Allow control on the Mac for headset input. The kill switch ⌃⌥⌘K
revokes headset input immediately. The host identity is a Keychain key (tag `com.cinetec.wvm.host.identity`)
plus `~/Library/Application Support/WVDHost/host-certificate.der`; deleting either makes headsets pair again.

## Encoder measurements

`wvd-host bench` on an Apple M3 Pro, macOS 27.2:

| Stage, 3840x2160 | p50 | p95 |
| --- | --- | --- |
| BGRA -> NV12 (GPU) | 1.36 ms | 1.82 ms |
| HEVC encode, VideoToolbox (submit -> last byte) | 11.7 ms | 12.2 ms |
| packetize + RS parity | 0.07 ms | 0.15 ms (p99 3.9 ms on IDRs, overlapped with sending) |

VideoToolbox's low-latency rate control was 1.8x slower per picture in this configuration (4K: 21.5 vs
11.7 ms; 1440p: 10.8 vs 6.1 ms; 1080p: 8.6 vs 4.0 ms), so the host defaults to standard rate control with
`DataRateLimits`.

| VideoToolbox, 1080p60, same 120-frame scene | Target | Encoded video | Luma PSNR | Encode p95 |
| --- | ---: | ---: | ---: | ---: |
| H.264 | 10 Mbit/s | 2.86 Mbit/s | 46.27 dB | 6.04–7.29 ms |
| HEVC | 10 Mbit/s | 2.94 Mbit/s | 52.16 dB | 3.96–4.64 ms |

Encoded video rate is calculated at 60 fps and excludes audio, packet headers and FEC.

Select the discovered host on Vision Pro and compare the pairing code on both devices. The [wire contract](../core/proto/WIRE.md) describes discovery, pairing, and media.

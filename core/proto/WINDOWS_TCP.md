# Windows P0 TCP compatibility

The primary product is the Vision Pro client connected to the Windows host. The client can now select
**Windows TCP (development)** to consume the existing host in `windows/`. This is separate from the
production QUIC/pairing contract in [WIRE.md](WIRE.md); a QUIC connection never falls back to plaintext.

The authoritative packed structs are in
[`protocol.h`](../../windows/src/common/include/wvd/protocol.h), with `#pragma pack(1)`. Every packet is
little-endian, starting with a 20-byte header: `u32 0x50445657`, `u16 version 1`, `u16 kind`,
`u32 payload_bytes`, `u64 sequence`. Payloads above 16 MiB are rejected before buffering their bodies.

| Packet | Kind | Fixed payload bytes | Client handling |
| --- | --- | --- | --- |
| Hello | 1 | 60 | Reports headset decoder capability; no authentication claim |
| MonitorLayout | 3 | 12 + 92 per monitor | Retains 64-bit Windows generations; maps them to local generations |
| CodecConfig | 5 | 44 + Annex B parameter sets | HEVC/H.264; extracts VPS/SPS/PPS or SPS/PPS for VideoToolbox |
| Input | 6 | 20 + 24 per event | Explicit opt-in; HID keys, pointer/button and vertical wheel |
| CursorShape | 8 | 32 + BGRA8 pixels | Bounds-checked and converted to premultiplied RGBA8 |
| CursorPos | 9 | 40 | Monitor index, shape version and layout generation checked |
| Media | 10 | 52 + Annex B AU | Converts to length-prefixed NALs, preserves reference order, uses existing decoder/recovery |
| ModeChange | 4 | 20 | Client to host: run the captured monitor at a listed size (the window's size menu) |
| RecoveryRequest | 12 | 16 | Maps to Windows IDR enum value 2 and the original 64-bit codec generation |
| DisplayModes | 18 | 24 + 16 per mode | Sizes the captured monitor offers; also the answer to a ModeChange |
| Heartbeat | 14 | 16 | Liveness only; QPC frequency/clock exchange is absent |
| Audio | 17 | 40 + PCM | Only after the hello asks for it; s16le interleaved at the PC's mix rate, resampled to 48 kHz on the headset |

Discovery is `_wvd._tcp` and the default port is **50001**. This mode is unencrypted and has no pairing;
the UI requires explicit selection/confirmation, defaults to view-only, and does not create a pinned host.
The Windows input helper's arming token remains a separate host-side requirement.

The Windows host lists all displays but encodes only its selected capture display. The first media unit
identifies that display; the client opens its window only. Input uses the original zero-based Windows
monitor index. A hidden window drops media and input; showing it requests a fresh IDR. Keyboard and mouse
state is released on focus/visibility loss using explicit key/button ups, because the P0 protocol does
not publish the input helper's generation. Disconnect also invokes the host's release/disarm path.

**Display size.** After `MonitorLayout` the host sends `DisplayModes`: `u64 layout_gen`, `u16 monitor`,
`u16 count`, `u32 request_id`, `u32 status`, `u32 reserved`, then per mode `u32 width`, `u32 height`,
`u32 refresh_mHz`, `u32 flags` (1 current, 2 native, 4 larger than the encode budget). The client maps it
to the `DisplayModes` envelope message and shows a size menu, grouped Standard / Wide / Ultrawide, when
there are two or more. A pick sends `ModeChange`: `u32 request_id`, `u16 monitor`, `u16 reserved`,
`u32 width`, `u32 height`, `u32 refresh_mHz` (0 = host's choice). The host answers with a `DisplayModes`
carrying that `request_id` and a status (1 applied, 2 unchanged, 3 failed, 4 unsupported). An applied size
then arrives the normal way: a new `MonitorLayout`, a new `CodecConfig` and a fresh `DisplayModes`. A list
never names a layout generation the client has not received; an older one is dropped. The change lasts for
the session; the host restores the monitor's mode at disconnect and the client re-requests its remembered
size on the next connection. The client keeps the layout's desktop size for the monitor (input and cursor
are in desktop pixels) even when the host encodes a smaller, scaled picture.

Audio is opt-in. When sound is on, the headset sets bit 0 of `HelloPayload.reserved` (`kHelloWantsAudio`).
An audio-capable Windows host can then capture loopback of the default playback device and send
`AudioPayload` packets (kind 17): u64 session, u64 seq, i64 QPC, u32 sample rate, u16 channels (2),
u16 format (1 = s16le), u32 frames (at most 2048), u32 reserved, then the samples. The PC's speakers keep
playing, because muting them needs the virtual audio endpoint driver of plan §4.3. The headset drops
malformed audio packets without closing the session. The hello may be repeated: the headset sends it
again whenever the wanted audio changes (sound switched on or off, every display hidden or shown), and the
host starts or stops audio on each one; only the audio bit of a repeated hello is acted on. A host
without audio only logs it. The first audio host build starts on a repeated hello but cannot stop until
disconnect, so the headset also drops audio it did not ask for. A 5.1/7.1 playback device is folded to
stereo on the host, and the capture follows the default playback device when it changes.

This mode supports hardware keyboards and the headset's special-key buttons. P0 has no Unicode text
event or horizontal wheel; software text entry is disabled rather than interpreted as HID keys. Bitrate
and frame rate are configured on the Windows host. QPC timestamps cannot be converted without their
frequency, so capture-to-present latency and RTT are shown as unavailable. The client bounds local
decode/presentation residence to 100 ms, without claiming a host capture deadline or measured network latency.

## Verification

`WindowsTCPTests` in the [Vision Pro client repository](https://github.com/snowzhangy/WVM) starts an independent Python TCP peer using
Python `struct` layouts matching the Windows C++ structs. It splits packets across TCP reads, sends
64-bit generations above 2^32, and streams real synthetic HEVC and H.264 access units through
`ClientSessionCore` and VideoToolbox. Checks cover captured-display selection, cursor channel order,
IDR requests, view-only defaults, stale/invalid input rejection, key/button release, hide/show recovery and
the display-size list/request round trip.
The peer also answers an audio-requesting hello with one malformed and 50 valid 44.1 kHz audio packets;
the test checks the opt-in, ordering, the malformed packet being dropped and the resampled length.
`WindowsTCPWireTests` checks split headers, size bounds, malformed payloads, enum/generation conversion and
the `DisplayModes` / `ModeChange` layouts.
Windows C++ static assertions lock the corresponding struct sizes.

These tests run on the development Mac. Real Windows GPU capture/encoding, input injection and Vision Pro
window/gesture behavior still require the physical PC/headset test; the fixture does not validate them.

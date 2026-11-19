# WVD backend-A wire protocol, version 1

Normative companion to [`wvd.proto`](wvd.proto) for hosts and the Vision Pro client.
The macOS reference implementation lives in [`macos/WVDKit`](../../macos/WVDKit).

## 1. Roles

The **host** owns monitors, captures, encodes and injects input. The **client** (the headset) decodes,
presents and sends input. One active session per host; further clients get `AUTH_BUSY`.

## 2. Transport

| Item | Value |
| --- | --- |
| Protocol | QUIC v1 (RFC 9000) with TLS 1.3, DATAGRAM extension (RFC 9221) required |
| ALPN | `wvd/1` |
| Default port | UDP 47800 (any port is allowed; Bonjour carries the real one) |
| Discovery | DNS-SD `_wvd._udp`, TXT `id=<host id>`, `v=1`. A hint only, never trusted |
| Host certificate | self-signed X.509, P-256 ECDSA. Pinned by the client as SHA-256 of the DER |
| Idle timeout | 15 s (the client pings every second) |
| Max datagram frame size | advertise 65535; the sender limits itself to the path's usable size |
| QoS | WMM AC_VI (`QOSTrafficTypeAudioVideo` on Windows, `.interactiveVideo` on Apple) |

The client must not trust the certificate through a CA. Before pairing it accepts any certificate and the
SAS comparison authenticates it (§7); afterwards it requires the pinned hash.

## 3. Flows and framing

| Flow | Opened by | First bytes | Carries |
| --- | --- | --- | --- |
| Control stream | client, first bidirectional stream | `WVD1` then `Envelope{hello}` | everything reliable except input |
| Input stream | client, lazily, bidirectional | `WVD1` then `Envelope{input_stream_open}` | `InputEvents`, `InputRelease` |
| Datagrams | client sends `FlowHello` (§4.3) | type byte | media shards, cursor positions, NACKs |

Every stream starts with the 4-byte preamble `57 56 44 31` ("WVD1") in each direction, followed by frames:

```
frame := u32 little-endian length (<= 4 MiB) || Envelope (protobuf, wvd.proto)
```

Input has its own stream so that a large `CursorShape` or `MonitorLayout` never delays a key press.

Network.framework surfaces a QUIC datagram flow on the server as just another connection, so the host
classifies each new flow by its first bytes: `WVD1` = stream, type `0x10` with the FlowHello magic =
datagram flow. msquic exposes datagrams per connection and needs no classification, but the client still
sends `FlowHello` and the host still confirms it (`SessionState.media_flow_bound = true`). The client
repeats `FlowHello` every 100 ms until confirmed, because datagrams are unreliable.

## 4. Datagram layouts (all integers little-endian)

### 4.1 Media shard, type `0x01` (host -> client), 48-byte header + payload

| Offset | Type | Field | Notes |
| --- | --- | --- | --- |
| 0 | u8 | type | `0x01` |
| 1 | u8 | flags | bit0 RANDOM_ACCESS (IRAP picture), bit1 PARITY, bit2 REFINEMENT, bit3 RETRANSMIT |
| 2 | u16 | monitor_id | |
| 4 | u32 | session_gen | from `HelloAck` |
| 8 | u32 | layout_gen | from `MonitorLayout` |
| 12 | u32 | codec_gen | from `CodecConfig` |
| 16 | u32 | pic_seq | every coded picture, starts at 1, +1 per picture, never reset in a session |
| 20 | u32 | content_ver | source desktop version; equal for refinement pictures of the same content |
| 24 | u32 | frame_bytes | access-unit length before padding |
| 28 | u16 | block_first_shard | global index of this block's first data shard |
| 30 | u16 | shard_index | within the block: `[0,k)` data, `[k,k+m)` parity |
| 32 | u8 | k | data shards in this block |
| 33 | u8 | m | parity shards in this block |
| 34 | u16 | shard_bytes | payload bytes per shard, identical for every shard of the picture |
| 36 | u16 | total_data_shards | N for the picture |
| 38 | u16 | deadline_budget_ms | presentation deadline = capture_time + budget (§9) |
| 40 | u64 | capture_time_us | host monotonic clock, microseconds |
| 48 | | payload | `shard_bytes` bytes |

Receivers reject a header unless: `shard_bytes > 0`, `k > 0`, `k + m <= 255`, `shard_index < k + m`,
`block_first_shard + k <= N`, the datagram holds `shard_bytes` payload bytes, and
`(N-1)*shard_bytes < frame_bytes <= N*shard_bytes`.

### 4.2 Cursor position, type `0x02` (host -> client), 32 bytes

| Offset | Type | Field |
| --- | --- | --- |
| 0 | u8 | type `0x02` |
| 1 | u8 | flags: bit0 visible |
| 2 | u16 | monitor_id |
| 4 | u32 | layout_gen |
| 8 | u32 | shape_ver (the `CursorShape` this position belongs to) |
| 12 | u32 | seq (newest wins, wrapping compare) |
| 16 | f32 | x: hotspot, monitor pixels |
| 20 | f32 | y |
| 24 | u64 | host_time_us |

Sent on every pointer move plus a keepalive at least every 100 ms. Shapes travel reliably as
`CursorShape` (premultiplied RGBA8, sRGB, top row first, at most 512x512). The client draws the cursor
locally every frame. `CursorShape.state` carries separate/embedded/hidden; a host
reports `EMBEDDED` only with positive evidence, and a client never hides its only cursor.

### 4.3 FlowHello, type `0x10` (client -> host), 12 bytes

`u8 0x10, u8 0, u16 0, u32 magic 0x47445657 ("WVDG"), u32 session_gen`.

### 4.4 NACK, type `0x11` (client -> host), 16 + 2n bytes

`u8 0x11, u8 n (<= 255), u16 monitor_id, u32 codec_gen, u32 pic_seq, u32 reserved 0,` then `n` x `u16`
global data-shard indices. `n = 0` asks for the whole picture (no shard of it arrived). The host
retransmits the requested data shards (flag RETRANSMIT) only while the picture can still meet its deadline.

### 4.5 Audio, type `0x03` (host -> client), 28-byte header + PCM

| Offset | Type | Field |
| --- | --- | --- |
| 0 | u8 | type `0x03` |
| 1 | u8 | format: 1 = signed 16-bit little-endian, interleaved |
| 2 | u8 | channels (1-8) |
| 3 | u8 | reserved 0 |
| 4 | u32 | session_gen |
| 8 | u32 | seq (+1 per packet, wrapping) |
| 12 | u32 | sample_rate (8000-192000 Hz) |
| 16 | u64 | host_time_us of the first sample |
| 24 | u16 | frames (1-2048) |
| 26 | u16 | reserved 0 |
| 28 | i16[] | frames x channels samples |

Hosts send audio only while the client's latest `StreamSettings.audio` (field 4) is true; it defaults to
false. Hosts can send 48 kHz stereo in 5 ms packets (240 frames, 988 bytes), sized to stay within
the negotiated datagram size for any channel count. Audio is uncompressed (1.5 Mbit/s at 48 kHz stereo)
and is not covered by FEC or NACK. The client drops packets of other sessions, malformed packets and late
or duplicate sequence numbers. It fills short gaps (up to 8 packets) with silence so timing holds, converts
any rate and channel count to 48 kHz stereo, and plays through a jitter buffer. That buffer primes to about
60 ms, re-primes with a fade after an underrun, and skips back to the target delay when more than
80 ms above it accumulates.

## 5. Forward error correction

Systematic Reed-Solomon over GF(2^8), primitive polynomial `x^8+x^4+x^3+x^2+1` (0x11D), Cauchy matrix:

```
parity_j = XOR_i  C[j][i] * data_i      C[j][i] = 1 / ((k + j) XOR i)     j < m, i < k, k + m <= 255
```

- A picture of `frame_bytes` is cut into `N = ceil(frame_bytes / shard_bytes)` data shards; the last one is
  zero-padded. Data shards are grouped into `B = ceil(N / 48)` blocks of near-equal size (the first
  `N mod B` blocks get one extra shard). `m` per block is the host's choice (reference:
  `ceil(k * fec_ratio)` clamped to `[1, 24]`, `fec_ratio` adapted from loss reports, 6-30%).
- Send order (recommended): data shards interleaved across blocks, then parity interleaved, so a Wi-Fi
  burst costs each block only a few shards. Data can leave before parity is computed.
- Any `k` shards of a block reconstruct it.

## 6. Media payload

- One datagram picture = one access unit of **4-byte big-endian length-prefixed NAL units**.
- Parameter sets are **not** in band: they are in `CodecConfig.parameter_sets` (HEVC: VPS, SPS, PPS; H.264:
  SPS, PPS), without start codes. Hosts whose encoders emit Annex B (NVENC, AMF) convert, strip parameter
  sets into `CodecConfig`, and raise `codec_gen` whenever they change (reference:
  `Bitstream.annexBToLengthPrefixed`).
- `codec_gen` changes on codec, profile, size, parameter-set or encoder-tier change. `CodecConfig` is sent on
  the control stream before any media of that generation; because streams and datagrams race, the client
  keeps one random-access picture of a not-yet-configured generation until its config arrives.
- RANDOM_ACCESS must be set exactly for IRAP pictures (HEVC NAL types 16-21, H.264 IDR); clients verify it.
- Profile: HEVC Main (8-bit 4:2:0), no B-frames, no periodic IDR, IDR on demand. Main10/HDR is a later
  version.

## 7. Pairing and authentication

TLS exporter secrets (RFC 8446 §7.5), 32 bytes each, on the QUIC connection:
`EXPORTER-wvd-sas-v1`, `EXPORTER-wvd-auth-v1`, `EXPORTER-wvd-pair-v1` (empty context).

```
cert    = SHA-256(host certificate DER)
pk      = client P-256 public key, X9.63 uncompressed (65 bytes)
SAS     = HKDF-SHA256(ikm = exporter_sas, salt = "wvd", info = "wvd-sas-v1" || cert || pk, L = 8)
          -> big-endian u64 mod 10^6, zero-padded to 6 digits
auth_tr = "wvd-auth-v1" || 0x00 || exporter_auth || cert || pk || utf8(device_id)
pair_tr = "wvd-pair-v1" || 0x00 || exporter_pair || cert || pk || utf8(device_id)
sig     = ECDSA P-256 / SHA-256, DER encoded
```

- **Paired client:** pins `cert`; `Hello.auth_signature = sig(auth_tr)`. The host verifies it with the stored
  key for `device_id` and answers `AUTH_OK`. Exporters differ per connection, so a signature cannot be replayed.
- **New client:** `Hello.request_pairing = true`. Only if the user opened the host's pairing window and the
  host is not locked out (5 failures / 10 min) does it answer `AUTH_PAIRING_STARTED` + `PairingChallenge`.
  Both screens show the SAS. A man in the middle terminates two different TLS sessions, so the codes differ.
  The user confirms on both: client sends `PairingConfirm{accepted, sig(pair_tr)}`, the host user clicks
  "Codes Match". Then the host stores `{device_id, pk}` and sends `PairingResult{success, cert}`; the client
  checks `cert` equals what it saw in TLS and pins it. The connection continues authenticated.
- Anything else fails closed (`AUTH_REJECTED` / `AUTH_PAIRING_REQUIRED`, then the connection closes).
  Unauthenticated connections are dropped after 30 s.

Input is a separate consent: after authentication the host shows "Allow <device> to control this
computer?" and reports `SessionState.input` (`CONSENT_PENDING` -> `ARMED` or `DENIED`). Video works either way.

## 8. Generations, deadlines and recovery

| Identifier | Owner | Rule |
| --- | --- | --- |
| `session_gen` | host, per connection | media with another value is dropped |
| `layout_gen` | host | media, cursor and input with an older value are dropped |
| `codec_gen` | host | new decoder per value; show only from a random-access picture |
| `input_gen` | host | events with another value are rejected; raised on every release, consent change or disarm |
| `pic_seq` | host | in-order delivery; a gap stops decoding until a random-access picture |

Client:
- Reassembly is bounded (8 pictures). A picture is abandoned `max(20, min(80, 3*RTT+15))` ms after its first
  shard, or when a later random-access picture is complete (then no recovery is needed).
- NACK once the picture is fully sent (a later picture arrived, or 2 ms of silence) and FEC cannot fill the
  gaps; at most 2 NACKs per picture, `max(2*RTT, 4 ms)` apart.
- After a loss, decode error or decoder overload: stop decoding, send `RecoveryRequest{kind = IDR,
  last_good_pic_seq, lost_pic_seq}`, repeat every `T_rec = 100 ms` until a random-access picture decodes.
  A host that supports reference invalidation/LTR may honour `RECOVERY_INVALIDATE` first; v1 clients ask for IDR.
- A decoded picture later than its deadline is dropped, never shown; the last good image stays and a stall
  indicator appears after 500 ms of pending recovery.

Host:
- IDR at stream start, on `RecoveryRequest` (ignored if an IDR newer than `last_good_pic_seq` left within
  150 ms), and when `Visibility` changes a monitor from hidden to visible; at most one per 100 ms.
  Replaying an unchanged visibility state does not request another IDR. The client retains each window's
  visibility across reconnects and sends it to the new session.
- Capture is newest-wins; at most one picture in the encoder; pacer backlog bounded.
- Static content: after 150 ms without change, re-encode the retained picture up to 4 times (REFINEMENT,
  same `content_ver`), then send nothing.
- Pace datagrams: spread a picture over at most half a frame interval, never above the configured link rate.
  (Network.framework drops QUIC datagrams it cannot send immediately; unpaced IDR bursts lose most shards.)
- If capture stops unexpectedly, release input owned by that session, disarm control, raise `input_gen`,
  send a `SessionState` with the reason, and close with `Goodbye.reason`. A capture failure is retryable;
  the reference host uses `capture stopped: <reason>`. A user ending sharing is intentional and must not
  trigger automatic reconnect; the reference host uses `Screen sharing ended on the host`.

## 9. Clocks

Both sides use a monotonic microsecond clock (`mach_absolute_time` base on Apple, QPC on Windows). The client
sends `Ping{client_send_us}` five times in the first 300 ms and then every second; the host answers
`Pong{client_send_us, host_recv_us, host_send_us}`. The client keeps the minimum-RTT sample of the last 16
(offset = host - client) and reports it with half that RTT as the error bound. Deadlines and
end-to-end latency are computed in the client clock from `capture_time_us`.

## 10. Colour contract

Capture BGRA8 as sRGB. Convert with the BT.709 matrix, full range, chroma siting left
(`chroma_sample_loc_type 0`): horizontal chroma filter [1 2 1]/4 centred on even luma columns, vertical
2-tap average. Signal `ColorInfo{primaries 1, transfer 13, matrix 1, full_range true, chroma_location 0}`
in `CodecConfig` and in the bitstream VUI. The client decodes to full-range bi-planar 4:2:0, converts with the
inverse matrix and writes gamma-encoded values through a non-sRGB view of an sRGB texture with a full mip
chain, sampled trilinear + 16x anisotropic. The reference kernels are `ColorPipelineShaders.source`.

## 11. Input

- Keys are USB HID usages (page 0x07), `down` true/false. Text from the software keyboard is `Text` (Unicode).
- Pointer coordinates are monitor pixels (float) with `monitor_id` + `layout_gen`; the host maps them through
  its own layout (per-monitor DPI, negative origins) and clamps to the monitor.
- `click_count = 0` lets the host derive multi-clicks from timing.
- Accept input only from the authenticated, armed session with the current `input_gen`, for a visible
  monitor with an active capture source. Drop every input kind while the monitor is hidden, including
  keyboard, text and scroll events. Pointer events must also match the monitor and current `layout_gen`.
- A visible -> hidden transition releases that session's held input, raises `input_gen`, and reports it in
  `SessionState`, fencing events already in flight. Repeating the same hidden state does not raise it again.
- `InputRelease` releases input owned by the session, raises `input_gen`, and reports `SessionState`.
  Input-stream closure or session disconnect also releases any input still owned by that session.
  The injector may be shared: a rejected or unused connection must never release another session's keys
  or buttons. After an explicit release, closing without further input requires no second release.
- The host kill switch releases all held input unconditionally, disarms control, raises `input_gen`, and
  reports `SessionState`.

## 12. Statistics

`ClientStats` every second per monitor (all counts per window): frames received/decoded/presented/late/
lost/superseded/present-failed, shards received/missing/recovered, NACKs, decode and capture-to-present
p50/p95, jitter, queue depths, clock offset and RTT. `frames_presented` (field 5) counts texture writes
completed successfully on the GPU, with capture-to-present latency sampled at completion.
`frames_present_failed` (uint32, field 22) counts frames that could not be submitted or whose GPU work
failed; they do not count as presented. The client keeps the last good image, shows the stall indicator,
and requests a fresh random-access picture after a presentation failure.
`HostStats` every second: capture/encode fps, bitrate, encode p50/p95,
queue depths, target bitrate, retransmitted shards, IDR count. Hosts adapt bitrate (x0.8 on loss, +8% after
three clean windows) and FEC ratio from `ClientStats`, and cap bitrate on `THERMAL_SERIOUS`/`CRITICAL`.

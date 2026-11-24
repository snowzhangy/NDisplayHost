# Game depth from the Windows host

Status: draft, 2026-11-22; side-by-side stereo added 2026-11-23 (section 11). For the Windows host engineer. The
Vision Pro side of depth is built (section 10); everything in sections 3–9 is host work in NDisplayHost.

## 1. Goal

Today the headset makes 3D from a flat picture by guessing depth with a model (ZipDepth, on the Neural Engine). The
guess is soft at edges, wrong in places and one prediction behind the picture (35–93 ms measured).

A game already has the exact depth of every pixel in its depth buffer. If the host sends that depth with the picture,
the headset can skip the model and warp with true depth:

- exact edges and shapes, no guessing;
- the depth belongs to the same frame as the colour, so it never lags;
- no model work on the headset.

When there is no game depth (the desktop, a menu, a game we cannot or may not hook), nothing changes: the headset
falls back to ZipDepth.

Depth is the second-best source, though. One picture plus depth cannot show what is behind an edge, cannot give each
eye its own reflections, and flattens particles, glass and the HUD. A game that renders one camera per eye has none
of those limits. Section 11 covers that path. It needs far less host work, and depth stays the path for every game
without a stereo mod, and for the PS5.

## 2. The change in one picture

```
 game process                       NDisplayHost (Windows)                         Vision Pro app
+--------------------+    shared   +--------------------------------------+       +---------------------------+
| hook DLL           |   texture   | depth -> inverse depth -> 8-bit      |  one  | decode                    |
|  Present           |-----------> | pack under the colour (strip)        | HEVC  | cut colour / depth apart  |
|  depth buffer copy |  + frame #  | encode as ONE picture, DEPTH_VALID   |------>| depth -> stereo warp      |
+--------------------+             +--------------------------------------+       | no depth -> ZipDepth      |
                                                                                   +---------------------------+
```

Depth is not an alpha channel and not a second stream. It is a grayscale strip drawn under the colour in the same
coded picture, so colour and depth are always from the same frame, arrive together and are lost together. Any
encoder works (NVENC, AMF, the Media Foundation hardware MFT, the software MFT), and the headset's decoder needs no
new feature.

## 3. Wire changes

All additive. A host or client built before this ignores the new fields. `wvdProtocolVersion` stays 1.

These fields are already in the app's hand-written codec (`mac/WVDKit/Sources/WVDCore/Wire/Messages.swift`). Please add
them to the canonical `core/proto/wvd.proto` and `core/proto/WIRE.md` with exactly these numbers.

### 3.1 Protobuf

```proto
message DecoderCaps {
  // ... fields 1-7 unchanged
  bool embedded_depth = 8;        // client can unpack a picture with a depth strip (EmbeddedDepthLayout)
}

message StreamSettings {
  // ... fields 1-4 unchanged
  repeated uint32 depth_monitors = 5;   // monitors whose viewer wants depth now (3D on). Empty = none.
}

message CodecConfig {
  // ... fields 1-9 unchanged
  EmbeddedDepthLayout embedded_depth = 10;  // present when every picture of this codec_gen carries a depth strip
}

enum DepthEncoding {
  DEPTH_INVERSE_NEAR_WHITE = 0;   // inverse depth, larger = nearer, normalised by the host (section 5)
}

message EmbeddedDepthLayout {
  uint32 picture_width = 1;    // the colour rectangle, at (0,0) of the coded picture
  uint32 picture_height = 2;
  uint32 strip_y = 3;          // first row of the depth strip
  uint32 map_width = 4;        // the depth map
  uint32 map_height = 5;
  uint32 slices = 6;           // the map is cut into this many horizontal bands, placed side by side in the strip
  DepthEncoding encoding = 7;
}
```

### 3.2 Media shard flag

`WIRE.md` §4.1, the `flags` byte: add **bit4 `DEPTH_VALID`**: this picture's strip holds depth for this picture.

- Set it identically on every shard of the picture: data, parity and retransmissions. The client reads it from
  whichever shard header the reassembler keeps.
- Clear it when the strip holds nothing this frame: a loading screen, a menu with no depth buffer, the hook missed
  the frame. Write the strip black in that case.

### 3.3 Rules

1. **Capability is not a request.** Pack a monitor only while the client advertised `embedded_depth` **and** the
   latest `StreamSettings.depth_monitors` names that monitor **and** the host has a depth source for it. Never pack
   because the capability alone is set.
2. **`codec_gen` changes whenever the layout does.** That covers packing starting or stopping, a size change, and a
   slice change. As always, the first picture of the new generation is an IDR, and `CodecConfig` goes first.
   - Every 3D toggle on the headset therefore costs one IDR, and so does hiding a 3D window (the app withdraws the
     request when a window hides).
   - To avoid IDR churn while a game runs, keep the layout up while the request stands and the hook is attached,
     and use `DEPTH_VALID` for frame-to-frame gaps.
   - Drop the layout (new `codec_gen`, no `embedded_depth`) when the request is withdrawn, or when the hook has
     been gone for about 2 s.
3. **`CodecConfig.width`/`height` are the coded size**, strip included. `MonitorLayout` keeps the monitor's real
   size; input and cursor mapping stay in monitor pixels.
4. **Never crop the strip with the SPS conformance window.** VideoToolbox applies the conformance window, so a strip
   outside it is cut off before the app sees it. The window may crop only the encoder's own alignment padding below
   the coded height in section 4.
5. **`DEPTH_VALID` without a layout means nothing.** The client ignores the bit when the generation has no
   `embedded_depth`.

## 4. Layout

For a monitor of `W`×`H` (both even):

```
map_width    = floor(W / 4)                     (3840 -> 960)
slices       = 4
map_height   = floor(H / 16) * 4                (2160 -> 540; divisible by slices)
slice_rows   = map_height / slices              (135)
strip_y      = align_up(H, 8) + 16              (2176: 16 guard rows under the colour)
coded width  = W
coded height = align_up(strip_y + slice_rows, 8)   (2312)
```

```
row 0     +----------------------------------------------+
          |                colour W x H                  |
row H     +----------------------------------------------+
          |  guard band: last colour row repeated        |  16 rows
row strip_y +-----------+-----------+-----------+--------+
          | map rows    | map rows  | map rows  | map    |  slice_rows rows
          | 0..134      | 135..269  | 270..404  | 405..  |
          +-----------+-----------+-----------+--------+
          (pad to coded height with black)
```

- Map row `y` is strip row `y % slice_rows` of slice `y / slice_rows`, and slice `i` starts at strip column
  `i * map_width`.
- **Strip chroma is neutral (128)** everywhere, so it costs almost nothing. Unused strip columns are black.
- **Guard band.** The 16 rows keep HEVC deblocking and intra prediction from smearing colour into depth and back.
  Fill them by repeating the last colour row, which compresses to nearly nothing.
- **Keep the map's shape the picture's shape**, within a pixel. The app fits the map to the picture rather than
  stretching it, so a mismatch shows up as a border with no depth. The formula above does that.
- **Slice seams** fall on multiples of `map_width` (960 is a multiple of 64). Deblocking can disturb about 3 map
  columns at each seam. Those are the map's left and right edges, where the app already fades parallax out.

| Monitor | Map | Coded picture | Luma samples | HEVC level 5.1 limit 8,912,896 |
| --- | --- | --- | --- | --- |
| 3840×2160 | 960×540 | 3840×2312 | 8,878,080 | fits |
| 2560×1440 | 640×360 | 2560×1552 | 3,973,120 | fits |
| 5120×1440 | 1280×360 | 5120×1552 | 7,946,240 | fits |
| 1920×1080 | 480×268 | 1920×1168 | 2,242,560 | fits |

**Frame rate:** at 4K the strip adds 7% to the luma sample rate. 4K at 60 Hz still fits level 5.1, just barely. 4K
at 90–100 Hz already needs level 5.2 without the strip. Respect `DecoderCaps.max_pixel_rate` with the strip counted
(3840×2312×96 Hz = 852 M samples/s against the headset's 1,000 M).

**Bitrate:** depth is smooth and compresses well. Expect roughly 5–15% of the picture's bits, a guess to be measured.
While packing, raise the target by about 10% within the client's `max_bitrate_kbps`, or the colour pays for it.
Where the encoder has a QP map (`EncoderCaps.qpMap`), the strip can take its own QP. Depth edges matter more than
its flat areas.

## 5. Depth encoding

The app needs **inverse depth, larger = nearer**, as an 8-bit luma value. It needs neither the near and far planes
nor the field of view, because the stereo warp is affine-invariant in inverse depth.

A perspective depth buffer is already affine in 1/z, whatever its near and far planes:

| The game uses | Depth buffer value `d` | Use `v =` |
| --- | --- | --- |
| reversed Z (most modern games): far = 0, near = 1 | affine in 1/z, increasing towards the camera | `d` |
| standard Z: near = 0, far = 1 | affine in 1/z, decreasing towards the camera | `1 - d` |

**Telling them apart.** The value the game passes to `ClearDepthStencilView` is the reliable hint: a clear to 0
means reversed Z, a clear to 1 means standard. The depth-stencil state's comparison function (`GREATER` against
`LESS`) confirms it. Keep a per-game override in the opt-in list (section 7).

**Precision is the reason the host normalises.** In a typical scene, `v` spans only a tiny slice of 0…1: with a
near plane of 0.1 m and the scene 2–50 m away, `v` runs from about 0.05 down to 0.002. Rounded straight to 8 bits,
that is about 12 of 256 levels. So per frame, at map resolution:

```
v  = reversed ? d : 1 - d                       // per depth texel
// Downsample 4x4 -> 1 texel by taking the MAX v (the nearest), so thin foreground objects survive.

lo, hi = 1st and 99th percentile of v over the frame
// Smooth lo and hi over time: exponential average, time constant about 0.5 s.
// Follow a jump at once when the centre moves by more than half the interval (a scene cut).
// Never let hi - lo fall below 1e-6.

q = saturate((v - lo) / (hi - lo))
Y = round(q * 255)                              // full range, as CodecConfig.color says today
```

- Sky and far-plane texels come out at or below `lo` and clamp to 0 (black), which is correct: "far".
- **Smoothing matters.** If `lo` and `hi` jump every frame, every depth value jumps with them and the 3D breathes.
  The app runs its own percentile smoother on top, so the host's only job is not to shake the scale.
- Orthographic depth (2D games, some UI passes) is linear in z rather than 1/z. It still sorts near from far
  correctly; leave it as is.

## 6. Host behaviour

Per monitor:

```
packing = client.embedded_depth && monitor in depth_monitors && depth_source_attached
if packing != was_packing or layout changed:
    new codec_gen; CodecConfig { ..., width/height = coded size, embedded_depth = layout }; IDR
each frame while packing:
    colour -> rows [0, H); guard band; strip from this frame's depth
    DEPTH_VALID = (depth for this exact frame was available and copied)
    if not DEPTH_VALID: strip black
```

- The colour conversion (`nv12_convert`) writes the colour into the top of a taller NV12 texture. The same pass, or a
  second small one, writes the guard band and strip. The encoder input is that whole texture.
- Keep `StreamSettings` handling as it is for the other fields. `depth_monitors` arrives in the same message, and
  every `StreamSettings` carries the full current list.

## 7. Getting the depth

### 7.1 Which process

- Watch the foreground fullscreen or borderless window on one of the host's virtual monitors.
- Read its process's loaded modules: `d3d11.dll`, `d3d12.dll`, `vulkan-1.dll`.
- Act only when **all** of these hold:
  - the user opted that executable in, from a list in the host UI. Ask once with "Send this game's depth to the
    headset for 3D?", then remember the answer;
  - no anti-cheat is loaded or installed with it. Skip on modules or folders such as `EasyAntiCheat*`, `BEService`,
    `BEClient*`, `vgk`/`vgc`, `EOSAntiCheat`, `xigncode`, `nProtect`, and keep that list updatable;
  - a client is requesting depth for that monitor right now.
- Never inject into anything else. Browsers, Office, Discord and Explorer all use D3D11.

### 7.2 The hook (D3D11 first)

A small signed DLL, installed with the host under Program Files and injected with `CreateRemoteThread` plus
`LoadLibraryW`. Vulkan uses an implicit layer instead (section 8, H4).

1. **Hook the device.** Hook `IDXGISwapChain::Present` (and `Present1`), plus the immediate context's
   `OMSetRenderTargets`, `ClearDepthStencilView` and the `Draw*` family, through the vtables.
2. **Pick the depth buffer.**
   - Count draw calls and vertices per bound depth-stencil per frame.
   - Prefer the busiest one whose size and aspect match the back buffer.
   - Remember the choice across frames; re-pick only when it stops being used.

   This is the heuristic of ReShade's generic depth add-on (`examples/09-depth/generic_depth_addon.cpp`,
   BSD 3-clause). Its code may be used with the copyright notice kept. **Do not copy OBS code** (GPL-2), whatever
   its injection helper looks like: the host is MIT.
3. **Copy before it is cleared.** Games clear depth more than once per frame. Copy the chosen buffer just before the
   clear that ends its main use (`ClearDepthStencilView`), or at Present if it is never cleared. Resolve MSAA first.
4. **Hand it over.**
   - The hook runs one tiny pass on the game's device: read the depth through a typeless view, apply `v` and the
     4×4 max downsample from section 5, and write `R16_UNORM` or `R32_FLOAT` into a texture shared with the host
     (NT handle, keyed mutex).
   - Next to it, a shared-memory header with: frame number, the QPC time of the Present, the reversed-Z guess,
     valid or not.
   - The host does the percentiles, normalisation, packing and encoding. Keep the hook as small as possible: a crash
     in it is a crash in the user's game.
5. **Match depth to colour.**
   - In H2 the colour still comes from desktop duplication. Pair each duplicated frame with the depth whose Present
     time equals `DXGI_OUTDUPL_FRAME_INFO.LastPresentTime`. No match means `DEPTH_VALID` is clear for that frame.
     Do not guess.
   - In H3 the hook also copies the back buffer at Present, as OBS game capture does. The host then encodes that
     colour instead of the duplicated desktop, and pairing is exact by construction.
6. **Same GPU only.** If the game renders on a different adapter from the host's encoder (some hybrid laptops), do
   not hook.
   The D3D12 add-on hands off colour in a same-adapter shared texture. The producer completes its colour
   copy and depth readback before publishing. The host copies colour into its own ring and signals a
   shared release fence, so the game can reuse the texture without waiting on the render thread. A
   user-scoped local pipe binds the anonymous frame mapping to the foreground game's process.
7. **Fail closed.** If anything is unexpected (wrong format, lost device, swap chain recreated, `Present` from a
   second thread), stop exporting depth and keep the game running. The host notices the missing frames and clears
   `DEPTH_VALID`.

### 7.3 Risks to state in the host UI

- **Anti-cheat.** Injecting a DLL into a game protected by anti-cheat can crash the game or get the user's account
  banned. Single-player only, opt-in only, and skip anything with anti-cheat. Say this in the opt-in prompt.
- **Antivirus.** Injecting into another process looks like malware behaviour. Sign the hook DLL and the host, and
  expect Defender or SmartScreen reports anyway in the first releases.
- **HUD and menus have no depth.** They take the depth of the scene behind them and appear to swim. That is a
  limit of this approach (section 9).

## 8. Phases

Rough effort for one engineer, a guess rather than a measurement. Each phase ends with something the headset can
show.

| Phase | Work | Rough size | Done when |
| --- | --- | --- | --- |
| **H1** | Wire fields (section 3), layout (4), encoding path (5–6), with a **synthetic depth source**: `--depth-test-pattern`, a left-to-right ramp plus a bright square that follows the mouse. No hook | 1–2 weeks | the acceptance list below passes on the headset |
| H2 | D3D11 hook: depth only, colour still from desktop duplication, pairing by Present time. Opt-in list, anti-cheat skip, signed DLL | 2–3 months | three single-player D3D11 titles show true depth; 30 minutes without a game crash |
| H3 | Colour from the hook at Present (exact pairing). D3D12 hook (command queue, resource states) | 2–3 months | no edge halo when panning quickly; three D3D12 titles |
| H4 | Vulkan implicit layer, registered at install and active for opted-in games at their next launch | 1–2 months | two Vulkan titles |
| H5 | Polish: QP map for the strip, bitrate step-up, a HUD mask (the open question below) | open | — |

**H1 is the important one.** It is the only way to check the headset side end to end before any hook exists, and it
flushes out every wire and layout mistake while they are cheap.

**Side-by-side stereo (section 11)** has its own phases, S1–S4. S1 needs no host work. S2 and S3 are small and can go
before H2: suggested order H1, S2, S3, then H2–H5, and S4 only if bitrate turns out to be the limit.

**H1 acceptance**, on a Debug build of the app (3D is Debug-only today):

1. With 3D off, the host sends no strip. The picture is the normal size and there is no new `codec_gen`.
2. Switch 3D on for a display. Within one IDR:
   - the statistics panel shows **Depth source: game depth from host 960×540** (at 4K);
   - the ramp reads as a tilted plane and the square stands out in front;
   - no strip is visible at the bottom of the screen.
3. Clear `DEPTH_VALID` for one frame in every second. The 3D holds with no flicker; the app keeps the host depth
   through gaps shorter than 250 ms.
4. Clear `DEPTH_VALID` for 3 s. The app moves to ZipDepth: the panel shows the model's name. When the bit comes
   back, it returns to host depth.
5. Switch 3D off, or hide the window. The host drops the layout within one IDR and the picture is the normal size.
6. Reconnect. The request is sent again and packing resumes.
7. Run 4K at 60 and at 96 Hz with the strip. Decode p95 in the statistics panel stays within 1 ms of the same stream
   without it.

## 9. Open questions

1. **HUD.** HUD and UI pixels have no depth of their own. One idea for H5:
   - copy the colour right after the last draw that uses the chosen depth buffer;
   - at Present, mark every pixel that changed since then as HUD.

   That still leaves how to tell the headset what to do with HUD pixels: a reserved value, or a second bit per texel
   in a wider strip. Not needed for H1–H4.
2. **10-bit.** With `hevc_main10` (later), the strip could carry 10-bit depth for free. Worth it only if banding
   shows on long flat surfaces.
3. **Bitrate step-up.** Ship the 10% figure only after measuring it in H1.
4. **The macOS host** is out of scope. It ignores the fields and never packs.

## 10. What the Vision Pro app already does

Built and unit-tested in this repository. It is Debug-only, like the rest of live 3D
(`docs/DEPTH_PRO_2_LIVE_3D_PLAN.md`).

| Step | Where |
| --- | --- |
| Advertises `DecoderCaps.embedded_depth` (Debug builds only) | `WVDCore/Session/ClientSessionCore.swift` |
| Sends `StreamSettings.depth_monitors`: a display joins while its 3D runs, and leaves when 3D stops or the window hides. Sent on change only, and again to each new session | `NDisplayApp/MonitorController.swift` `requestHostDepth`, `NDisplayAdapters/DesktopAdapters.swift` |
| Parses `CodecConfig.embedded_depth` and the `DEPTH_VALID` bit | `WVDCore/Wire/Messages.swift`, `Datagrams.swift`, `FEC/Reassembler.swift` |
| After decode, cuts the colour out (GPU blit into a decoder-identical buffer) and reads the strip into a float16 map (vImage). Everything downstream (presentation, smooth motion, upscaling) sees only the colour | `WVDCore/Media/Depth/EmbeddedDepthSplitter.swift`, `WVDCore/Session/MediaReceiver.swift` |
| Uses host depth for the stereo warp while it keeps arriving (250 ms hold), skips the model for those pictures, and falls back to ZipDepth otherwise, restarting the model's history when it does. Each picture's own map is used at once; its near–far interval is measured on a background queue | `NDisplayApp/Depth3D.swift` `Depth3DDriver`, `EmbeddedDepthRangeTracker` |
| Shows "Depth source" in the statistics panel, and writes a `depth3d[n] host …` line to the performance log | `NDisplayApp/MonitorView.swift`, `Depth3D.swift` |

**Tests:**
- `WVDCoreTests/EmbeddedDepthTests.swift`:
  - wire round trips, including an older peer;
  - layout checks;
  - the flag through packetize and reassemble, parity first;
  - a byte-exact colour cut;
  - slice reassembly;
  - full-range and video-range mapping;
  - wrapping the map for the warp.
- `NDisplayAdapterTests/AdapterSettingsTests.swift`: the request surviving every other settings change.

## 11. True stereo from side-by-side sources

**Decision, 2026-11-23.** The best 3D comes from a game that renders one camera per eye. Depth (sections 1–10) stays
the path for games without a stereo mod, and for the PS5, whose Remote Play stream is one 2D view. The view stays a
flat or curved screen. A full-VR or 180° immersive mode is out of scope: a wide view moved by a gamepad camera makes
people motion sick, and ALVR already covers full VR.

### 11.1 Where the stereo comes from

Third-party tools that the user installs. The host neither bundles nor injects them.

| Tool | Covers | Output |
| --- | --- | --- |
| [VRto3D](https://github.com/oneup03/VRto3D), a SteamVR driver (LGPL-3.0; needs Steam and SteamVR, no headset) | anything that renders for SteamVR or OpenXR: UEVR (Unreal 4/5), REFramework (RE Engine), UUVR (Unity, early), Luke Ross's R.E.A.L. mods, native VR games | a fullscreen side-by-side or top-bottom window on the monitor named by its `display_index` |
| [geo-11](https://helixmod.blogspot.com/2022/06/announcing-new-geo-11-3d-driver.html), a 3D Vision replacement | DX11 games, often with Helix shader fixes | the game's own window, with `direct_mode = sbs` |

- The VRto3D keys that matter: `fov`, `aspect_ratio`, `render_width` and `render_height` (per eye), `depth` and
  `convergence`. Leave `pitch_enable` and `yaw_enable` off, so the gamepad moves the camera and the head does not.
- SteamVR has no HDR, so these streams are SDR.
- The anti-cheat rule of section 7.3 applies to these injectors too: single-player only. It is the user's choice,
  and the host does nothing about it.

### 11.2 S1: works with today's host

These tools draw both eyes into an ordinary window, so the host captures and encodes it like any other desktop. The
app has a per-display **3D from** setting (depth, side-by-side, half side-by-side, top-bottom, half top-bottom, plus
Swap eyes) and gives each half of the picture to one eye. Built in this repository, Debug only like the rest of 3D:

| Step | Where |
| --- | --- |
| Eye rectangles, and the shape each eye is shown at, per layout | `WVDCore/Media/Depth/StereoPacking.swift` |
| The cut: NV12 → two eye surfaces, chroma kept inside each eye so the seam stays clean, own mips per eye | `nd_stereo_unpack` in `DepthShaders.swift`, `StereoUnpacker`, `StereoPresentation.preparePacked` |
| The stage: no model, no 3D slot, no depth request; never written flat (both eyes squeezed) while it starts | `NDisplayApp/Depth3D.swift` `PackedStereoDriver`, `MonitorController.swift` |
| Screen shaped to one eye (`MonitorController.aspect`); PC streams only | `MonitorController.swift`, `MonitorView.swift` |

For now, upscaling is off in these layouts and the locally drawn cursor is hidden. Input stays in monitor pixels.

Host notes, no code:

- The stereo window must be on a monitor the host captures: set VRto3D's `display_index` to that virtual monitor.
- While a display shows a side-by-side or top-bottom picture, the app does not request depth for it (the display
  leaves `depth_monitors`), so the host sends no strip. Nothing to change.
- Half side-by-side at 3840×2160 (1920×2160 per eye) fits every encoder and today's `--max-pixels` default.

### 11.3 S2: full resolution per eye

Full side-by-side at 4K per eye is a 7680×2160 monitor.

The Windows host exposes this mode through **Stereo layout → Side by side** or
`--stereo-layout sbs`. When it needs half side-by-side, it squeezes the picture
horizontally and advertises the resulting layout. **Half side by side** describes
a source that already supplies the squeezed picture. The [Windows setup](../windows/README.md#stream-a-side-by-side-source)
covers both cases.

The **Stereo output resolution** control offers 720p, 1080p, 1440p and 2160p
packed presets for half side-by-side and the corresponding double-width presets
for full side-by-side. A 2560×1440 half side-by-side stream carries two
1280×1440 eye images. Vision Pro stretches each eye horizontally for presentation.

- **Virtual monitor modes.** Offer 7680×2160 (3840×2160 per eye) and 5120×1440 (2560×1440 per eye).
- **Encode budget.** The `--max-pixels` default is 8,294,400 (3840×2160), so a 7680×2160 desktop is scaled down for
  encode and its mode is marked `kModeScaledForEncode`. Raise the cap to 16,588,800 when the encoder takes width
  7680. Scaling keeps working meanwhile: the whole picture is scaled, so the two halves stay aligned.
- **Encoder.** 7680×2160 is 16.6 M luma samples, over HEVC level 5.1's 8.9 M, so it needs level 6. NVENC takes widths
  up to 8192. AMF on the Z13 (Strix Halo) is unverified: probe it, and fall back to half side-by-side.
- **Headset decode budget.** The app advertises at most 8192×4320 and 1,000 M luma samples a second. 7680×2160 at
  60 Hz is 995 M, which only just fits; 90 or 100 Hz does not. Cap full side-by-side at 60 Hz, or use half
  side-by-side at higher rates.
- **Bitrate.** About twice the pixels of a 4K desktop. Measure before raising any default.

The Windows hardware probe encoded 7680×2160 at a configured 60 fps through the AMD
Media Foundation encoder and verified Level 6 in the SPS. With a simulated headset
limited to 4096 pixels wide, the same source produced 3840×2160 half side-by-side at
a configured 90 fps with Level 5.2. Both probes verified the layout and first IDR.
The 7680×2160 IddCx mode and live headset playback still need hardware validation.

### 11.4 S3: the stereo layout on the wire

The Windows host and NDisplay Debug app use field 11. With
**3D from → Automatic**, the app uses the host's layout and sizes the screen
from the decoded picture. A manual layout selection overrides it.

```proto
enum StereoLayout {
  STEREO_NONE = 0;
  STEREO_SIDE_BY_SIDE = 1;        // left eye in the left half, at full width per eye
  STEREO_SIDE_BY_SIDE_HALF = 2;   // each eye squeezed to half the width
  STEREO_TOP_BOTTOM = 3;          // left eye in the top half
  STEREO_TOP_BOTTOM_HALF = 4;
}

message CodecConfig {
  // ... fields 1-10 unchanged
  StereoLayout stereo_layout = 11;
}
```

- First version: the host sets it when the user marks a monitor as stereo in the host UI. Detecting a VRto3D or
  geo-11 window can come later.
- A change of layout is a new `codec_gen`, like any other layout change.
- Never together with `embedded_depth`: a packed stereo picture needs no depth.
- The app treats the flag as the default and the user's own setting overrides it.

### 11.5 S4: MV-HEVC (NVIDIA only, later)

- NVENC in Video Codec SDK 13.0 encodes MV-HEVC: two views, 8- or 10-bit 4:2:0, up to 27% fewer bits than two
  separate streams, on Turing (RTX 20) and newer
  ([NVIDIA](https://developer.nvidia.com/blog/enabling-stereoscopic-and-3d-views-using-mv-hevc-in-nvidia-video-codec-sdk-13-0/)).
  FFmpeg's nvenc supports it.
- The Vision Pro decodes MV-HEVC in hardware: it is what spatial video and Apple Immersive Video use.
- It needs a `DecoderCaps` bit, a `CodecConfig` codec value, a decode path in the app, and the host taking the two
  eyes as separate textures rather than one packed picture.
- AMD's AMF documents no MV-HEVC encode, so the Z13 stays on side-by-side.
- Worth doing only after S2 shows that bitrate is the limit.

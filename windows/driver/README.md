# WVD IddCx driver

UMDF virtual-monitor provider. **The driver has not been compiled or installed
on the development PC, which has the Windows SDK but no WDK.**

The source follows Microsoft's
[IndirectDisplay sample](https://github.com/microsoft/Windows-driver-samples/tree/main/video/IndirectDisplay/IddSampleDriver)
for UMDF/IddCx initialization, callbacks and package layout. The project targets
Windows 11 x64, UMDF 2.33 and IddCx 1.4. A WDK build and InfVerif remain required
before treating the package as usable.

Implemented source paths:

* Adapter initialization advertises one monitor and endpoint diagnostics.
* Arrival publishes EDID and reference modes. Count-only mode queries succeed
  and preferred-mode indices are supplied. The modes are 16:9 up to
  3840×2160 (60/90/120 Hz at 1080p), 21:9 (2560×1080, 3440×1440, 5120×2160)
  and 32:9 (3840×1080, 5120×1440), all listed in the headset's size menu.
* A 128-byte EDID base block has standard timings at bytes 38–53, detailed
  timings at byte 54, a two-byte 10 kHz pixel clock, correct active/blanking
  fields, physical dimensions, monitor name and checksum. `Edid.cpp` builds and
  is tested separately in the user-mode CTest suite.
* Swapchain buffers are drained using `IddCxSwapChainReleaseAndAcquireBuffer`
  and `IddCxSwapChainFinishedProcessingFrame`; a stopped monitor joins its drain
  thread. Device creation uses the adapter provided by AssignSwapChain.
* The driver does not encode video. Desktop Duplication in `wvd-media` captures
  the output. Hardware-cursor setup, HDR, dynamic monitor creation, and Path S
  shared-surface transport remain unimplemented.

`RenderAdapterLuid` in the device registry key, when present as REG_BINARY, is
only a diagnostic hint. The actual swapchain GPU comes from AssignSwapChain.
The owner currently does not write this value or call an adapter-pinning API.

## WDK build

Use Visual Studio with a matching Windows SDK/WDK and the UMDF tools:

```powershell
msbuild wvdidd.vcxproj /p:Configuration=Release /p:Platform=x64
```

The output is **WvdIdd.dll**, its stamped INF, and a catalog/package. The project
declares `IndirectDisplayDriver`, IddCx versions, D3D/DXGI libraries, C++17, and
packaging. The INF imports the WUDFRD.NT sections and declares source media.
Run the WDK package/INF validation and driver verification tools; the user-mode
CMake build does not validate the WDF/IddCx callbacks or INF.

Use a dedicated test machine and the existing `scripts/dev-install-driver.ps1`
for lab installation after reviewing that script. Test signing and Secure Boot
requirements depend on the machine's configuration; production distribution
requires a signed driver package. No driver-signing or boot-security settings
are changed by the normal build or `start-dev.ps1`.

# macOS host

The **WVD Host** menu bar app streams a selected display or a synthetic test pattern via QUIC. The sender uses ScreenCaptureKit, Metal, and VideoToolbox. macOS prompts for Screen Recording permission for real-display capture and Accessibility permission before injecting input.

Use macOS 26 or later with Xcode and Swift 6.2 or later. From this directory:

```sh
open WVD.xcodeproj
xcodebuild -project WVD.xcodeproj -scheme WVDHost -destination 'platform=macOS' CODE_SIGNING_ALLOWED=NO build
swift test --package-path WVDKit
```

Select your development team in Xcode for a signed app. `xcodegen generate` can regenerate the checked-in project from `project.yml`.

The CLI is also available from the same package:

```sh
swift run --package-path WVDKit -c release wvd-host serve
swift run --package-path WVDKit -c release wvd-host serve --display 0 --allow-input
swift run --package-path WVDKit -c release wvd-host bench
```

Select the discovered host on Vision Pro and compare the pairing code on both devices. The [wire contract](../core/proto/WIRE.md) describes discovery, pairing, and media. Client interoperability tests live in the WVM client repository and can run against this checkout.

// swift-tools-version: 6.2
import PackageDescription

let package = Package(
    name: "WVDHost",
    platforms: [.macOS(.v26), .visionOS(.v26)],
    products: [
        .library(name: "WVDAppleCore", targets: ["WVDAppleCore"]),
        .library(name: "WVDHostKit", targets: ["WVDHostKit"]),
        .executable(name: "wvd-host", targets: ["wvd-host"]),
    ],
    targets: [
        .target(name: "WVDAppleCore"),
        .target(name: "WVDHostKit", dependencies: ["WVDAppleCore"]),
        .executableTarget(name: "wvd-host", dependencies: ["WVDHostKit", "WVDAppleCore"]),
        .testTarget(name: "WVDHostKitTests", dependencies: ["WVDHostKit"]),
    ]
)

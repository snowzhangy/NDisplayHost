# Host core

[`proto/`](proto/) contains the [versioned QUIC wire contract](proto/WIRE.md), its [protobuf schema](proto/wvd.proto), and the [Windows development TCP contract](proto/WINDOWS_TCP.md).

`wvd_wire_core` is the portable C++17 implementation of WIRE v1 framing, message encoding, and forward error correction. The Windows QUIC host links it, and a future C++ host can link the same target. It builds with CMake 3.24 or later and a C++17 toolchain. From the repository root:

```sh
cmake -S core -B build/core
cmake --build build/core --config Release
ctest --test-dir build/core -C Release --output-on-failure
```

The platform hosts own their entry points, capture, encode, input, transport bindings, and packaging. The Apple wire and media utilities shared with the client live in `macos/HostKit/Sources/WVDAppleCore`.

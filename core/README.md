# Host core

[`proto/`](proto/) contains the [versioned QUIC wire contract](proto/WIRE.md), its [protobuf schema](proto/wvd.proto), and the [Windows development TCP contract](proto/WINDOWS_TCP.md).

The platform hosts own their entry points, capture, encode, input, transport bindings, and packaging. The Windows and Apple implementations currently keep their wire codecs and FEC alongside their platform code. The Apple wire and media utilities shared with the client live in `macos/WVDKit/Sources/WVDAppleCore`.

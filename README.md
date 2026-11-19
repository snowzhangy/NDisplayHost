# NDisplayHost

Desktop streaming hosts for the NDisplayApp Vision Pro client. The Windows host ships as **NDisplayHostWin**. The macOS host has a menu bar app and a command-line sender.

| Directory | Responsibility |
| --- | --- |
| [`core/`](core/) | Platform-independent wire contract, C++ framing, message encoding, and forward error correction. |
| [`windows/`](windows/) | Windows capture, encode, streaming, input, control panel, and virtual display driver. |
| [`macos/`](macos/) | macOS capture, encode, streaming, input, menu bar app, and CLI. |
| [`linux/`](linux/) | Reserved for the Linux host. |

Build each host from its own checkout using the [Windows instructions](windows/README.md) or the [macOS instructions](macos/README.md). The Vision Pro app is developed in [NDisplayApp](https://github.com/snowzhangy/NDisplayApp).

The [wire specification](core/proto/WIRE.md) and [schema](core/proto/wvd.proto) are canonical here. Update both with wire changes and exercise the client interoperability tests before relying on a change across repositories.

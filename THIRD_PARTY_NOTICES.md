# Third-party components

- [MsQuic](https://github.com/microsoft/msquic) is fetched at the revision specified in `windows/scripts/build-msquic.ps1`. The Windows QUIC build includes its license with the runtime DLL.
- The Windows indirect display driver follows Microsoft's [IddCx sample](https://github.com/microsoft/Windows-driver-samples/tree/main/video/IndirectDisplay/IddSampleDriver). Its documentation records the upstream reference.
- The optional NVIDIA Video Codec SDK is supplied by the developer at build time.

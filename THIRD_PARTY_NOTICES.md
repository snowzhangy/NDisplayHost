# Third-party components

- [MsQuic](https://github.com/microsoft/msquic) is fetched at the revision specified in `windows/scripts/build-msquic.ps1`. The Windows QUIC build includes its license with the runtime DLL.
- The Windows indirect display driver follows Microsoft's [IddCx sample](https://github.com/microsoft/Windows-driver-samples/tree/main/video/IndirectDisplay/IddSampleDriver). Its documentation records the upstream reference.
- The optional NVIDIA Video Codec SDK is supplied by the developer at build time.

For a future AGPL-licensed helper, distribute it as a separate component with
its applicable AGPL license and copyright notices. Publish the complete
corresponding source for the exact helper build, including modifications and
the scripts needed to build and install it, and link to the source next to the
binary download. Follow the source-offer requirements for remote users when
they apply. See the [GNU Affero General Public License](https://www.gnu.org/licenses/agpl-3.0.html).

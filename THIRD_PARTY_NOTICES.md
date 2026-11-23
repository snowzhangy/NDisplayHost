# Third-party components

- [MsQuic](https://github.com/microsoft/msquic) is fetched at the revision specified in `windows/scripts/build-msquic.ps1`. The Windows QUIC build includes its license with the runtime DLL.
- The Windows indirect display driver follows Microsoft's [IddCx sample](https://github.com/microsoft/Windows-driver-samples/tree/main/video/IndirectDisplay/IddSampleDriver). Its documentation records the upstream reference.
- The optional NVIDIA Video Codec SDK is supplied by the developer at build time.
- The optional game-depth add-on builds against the [ReShade 6.8.0 SDK](https://github.com/crosire/reshade/tree/v6.8.0/include), copyright 2014 Patrick Mours, under the [BSD 3-Clause license](https://github.com/crosire/reshade/blob/v6.8.0/LICENSE.md).

For a future AGPL-licensed helper, distribute it as a separate component with
its applicable AGPL license and copyright notices. Publish the complete
corresponding source for the exact helper build, including modifications and
the scripts needed to build and install it, and link to the source next to the
binary download. Follow the source-offer requirements for remote users when
they apply. See the [GNU Affero General Public License](https://www.gnu.org/licenses/agpl-3.0.html).

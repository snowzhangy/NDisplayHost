import CoreMedia
import CoreVideo
import Foundation
import VideoToolbox

/// Synchronous, real-time VideoToolbox decoder for one `codec_gen` (plan §5, §6.6).
///
/// A new `codec_gen` always means a new `VideoDecoder`: an IDR alone does not change how the codec is
/// interpreted. Output is IOSurface-backed, Metal-compatible bi-planar 4:2:0 whose range matches the
/// stream's VUI, so no implicit range conversion happens (plan §6.7).
public final class VideoDecoder: @unchecked Sendable {
    public enum DecodeError: Error, Equatable {
        case session(OSStatus)
        case blockBuffer(OSStatus)
        case sampleBuffer(OSStatus)
        case decode(OSStatus)
        case noImage
        /// The OS invalidated the session (e.g. app backgrounded). Recreate the decoder.
        case sessionInvalidated
    }

    public let config: CodecConfig
    public let pixelFormat: OSType
    private let format: CMVideoFormatDescription
    private var session: VTDecompressionSession?

    public init(config: CodecConfig) throws {
        self.config = config
        self.format = try Bitstream.formatDescription(for: config)
        let fullRange = config.color?.fullRange ?? true
        self.pixelFormat = fullRange ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        try createSession()
    }

    deinit {
        if let session { VTDecompressionSessionInvalidate(session) }
    }

    private func createSession() throws {
        let destination: [CFString: Any] = [
            kCVPixelBufferPixelFormatTypeKey: pixelFormat,
            kCVPixelBufferMetalCompatibilityKey: true,
            kCVPixelBufferIOSurfacePropertiesKey: [CFString: Any](),
        ]
        var spec: [CFString: Any] = [:]
        #if os(macOS)
        spec[kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder] = true
        #endif
        var s: VTDecompressionSession?
        let status = VTDecompressionSessionCreate(
            allocator: kCFAllocatorDefault, formatDescription: format,
            decoderSpecification: spec.isEmpty ? nil : spec as CFDictionary,
            imageBufferAttributes: destination as CFDictionary, outputCallback: nil, decompressionSessionOut: &s)
        guard status == noErr, let s else { throw DecodeError.session(status) }
        VTSessionSetProperty(s, key: kVTDecompressionPropertyKey_RealTime, value: kCFBooleanTrue)
        session = s
    }

    /// Recreates the hardware session after invalidation (same codec_gen, same format).
    public func recreateSession() throws {
        if let session { VTDecompressionSessionInvalidate(session) }
        session = nil
        try createSession()
    }

    /// Decodes one access unit synchronously. The AU buffer is wrapped without copying.
    public func decode(_ au: AUBuffer) -> Result<CVPixelBuffer, DecodeError> {
        guard let session else { return .failure(.sessionInvalidated) }

        var source = CMBlockBufferCustomBlockSource(
            version: kCMBlockBufferCustomBlockSourceVersion,
            AllocateBlock: nil,
            FreeBlock: { refCon, _, _ in
                if let refCon { Unmanaged<AUBuffer>.fromOpaque(refCon).release() }
            },
            refCon: Unmanaged.passRetained(au).toOpaque())
        var block: CMBlockBuffer?
        var status = CMBlockBufferCreateWithMemoryBlock(
            allocator: kCFAllocatorDefault, memoryBlock: au.pointer, blockLength: au.length,
            blockAllocator: kCFAllocatorNull, customBlockSource: &source, offsetToData: 0, dataLength: au.length,
            flags: 0, blockBufferOut: &block)
        guard status == noErr, let block else {
            Unmanaged<AUBuffer>.fromOpaque(source.refCon!).release()
            return .failure(.blockBuffer(status))
        }

        var sample: CMSampleBuffer?
        var size = au.length
        status = CMSampleBufferCreateReady(
            allocator: kCFAllocatorDefault, dataBuffer: block, formatDescription: format, sampleCount: 1,
            sampleTimingEntryCount: 0, sampleTimingArray: nil, sampleSizeEntryCount: 1, sampleSizeArray: &size,
            sampleBufferOut: &sample)
        guard status == noErr, let sample else { return .failure(.sampleBuffer(status)) }

        // Synchronous decode: the handler runs before DecodeFrame returns (or before WaitForAsynchronousFrames).
        final class Box: @unchecked Sendable { var result: Result<CVPixelBuffer, DecodeError> = .failure(.noImage) }
        let box = Box()
        var infoFlags = VTDecodeInfoFlags()
        status = VTDecompressionSessionDecodeFrame(session, sampleBuffer: sample, flags: [._1xRealTimePlayback],
                                                   infoFlagsOut: &infoFlags) { decodeStatus, _, image, _, _ in
            if decodeStatus != noErr {
                box.result = .failure(.decode(decodeStatus))
            } else if let image {
                box.result = .success(image)
            }
        }
        if infoFlags.contains(.asynchronous) {
            VTDecompressionSessionWaitForAsynchronousFrames(session)
        }
        if status == kVTInvalidSessionErr {
            return .failure(.sessionInvalidated)
        }
        if status != noErr, case .failure(.noImage) = box.result {
            return .failure(.decode(status))
        }
        return box.result
    }
}

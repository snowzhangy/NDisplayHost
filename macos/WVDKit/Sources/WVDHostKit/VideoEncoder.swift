#if os(macOS)
import CoreMedia
import CoreVideo
import Foundation
import VideoToolbox
import WVDAppleCore

/// Hardware HEVC encoder (VideoToolbox), tuned for interactive desktop streaming (plan §4.2 common profile):
/// real time, speed over quality, no frame reordering (no B-frames), no frame delay, no periodic keyframes
/// (IDR on demand), size-capped bursts, BT.709 / sRGB / full-range VUI per the colour contract (plan §6.7).
///
/// Rate control: measured on an M3 Pro (`wvd-host bench`), VideoToolbox's *low-latency* rate control is
/// about 1.8x slower per picture than the standard one in this configuration (3840x2160: p50 21.5 vs
/// 11.8 ms; 2560x1440: 10.8 vs 6.1 ms; 1920x1080: 8.6 vs 4.0 ms), content-independent, so the standard
/// rate control with `DataRateLimits` is the default. Low-latency RC gives tighter IDR sizes (152 vs
/// 342 KiB at 4K) and stays available via `Settings.lowLatencyRateControl`.
public final class VideoEncoder: @unchecked Sendable {
    public struct Settings: Sendable, Equatable {
        public var width: Int
        public var height: Int
        public var fps: Int
        public var bitrateBps: Int
        public var codec: CodecKind = .hevc
        public var lowLatencyRateControl = false

        public init(width: Int, height: Int, fps: Int, bitrateBps: Int, codec: CodecKind = .hevc, lowLatencyRateControl: Bool = false) {
            self.width = width; self.height = height; self.fps = fps; self.bitrateBps = bitrateBps; self.codec = codec
            self.lowLatencyRateControl = lowLatencyRateControl
        }
    }

    public struct Output: @unchecked Sendable {
        public var data: Data               // length-prefixed NAL units (4-byte big endian)
        public var parameterSets: [[UInt8]]
        public var randomAccess: Bool
        public var encodeMicros: UInt64
        public var tag: UInt64
    }

    public enum EncoderError: Error { case session(OSStatus), encode(OSStatus) }

    public private(set) var settings: Settings
    /// Whether the session actually runs with low-latency rate control.
    public private(set) var lowLatencyRateControl = false
    private var session: VTCompressionSession?
    private var frameIndex: Int64 = 0

    public init(settings: Settings) throws {
        self.settings = settings
        try createSession()
    }

    deinit {
        if let session {
            VTCompressionSessionCompleteFrames(session, untilPresentationTimeStamp: .invalid)
            VTCompressionSessionInvalidate(session)
        }
    }

    private func createSession() throws {
        let codecType = settings.codec == .h264 ? kCMVideoCodecType_H264 : kCMVideoCodecType_HEVC
        let source: [CFString: Any] = [
            kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
            kCVPixelBufferWidthKey: settings.width, kCVPixelBufferHeightKey: settings.height,
            kCVPixelBufferIOSurfacePropertiesKey: [CFString: Any](),
            kCVPixelBufferMetalCompatibilityKey: true,
        ]
        func make(lowLatency: Bool) -> (OSStatus, VTCompressionSession?) {
            var spec: [CFString: Any] = [kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: true]
            if lowLatency { spec[kVTVideoEncoderSpecification_EnableLowLatencyRateControl] = true }
            var s: VTCompressionSession?
            let status = VTCompressionSessionCreate(allocator: nil, width: Int32(settings.width), height: Int32(settings.height),
                                                    codecType: codecType, encoderSpecification: spec as CFDictionary,
                                                    imageBufferAttributes: source as CFDictionary, compressedDataAllocator: nil,
                                                    outputCallback: nil, refcon: nil, compressionSessionOut: &s)
            return (status, s)
        }
        var (status, s) = make(lowLatency: settings.lowLatencyRateControl)
        lowLatencyRateControl = settings.lowLatencyRateControl && status == noErr
        if status != noErr && settings.lowLatencyRateControl { (status, s) = make(lowLatency: false) }
        guard status == noErr, let s else { throw EncoderError.session(status) }

        func set(_ key: CFString, _ value: Any) { VTSessionSetProperty(s, key: key, value: value as CFTypeRef) }
        set(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue!)
        set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse!)
        set(kVTCompressionPropertyKey_ProfileLevel, settings.codec == .h264 ? kVTProfileLevel_H264_High_AutoLevel : kVTProfileLevel_HEVC_Main_AutoLevel)
        set(kVTCompressionPropertyKey_ExpectedFrameRate, settings.fps)
        set(kVTCompressionPropertyKey_MaxKeyFrameInterval, Int32.max)
        set(kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, 0)  // 0 = no time-based keyframes
        set(kVTCompressionPropertyKey_AverageBitRate, settings.bitrateBps)
        set(kVTCompressionPropertyKey_MaxFrameDelayCount, 0)
        set(kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanTrue!)
        set(kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2)
        set(kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_sRGB)
        set(kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2)
        // Cap instantaneous bursts at ~1.5 frames of the average rate so an IDR cannot flood Wi-Fi.
        let bytesPerSecond = settings.bitrateBps / 8
        set(kVTCompressionPropertyKey_DataRateLimits, [bytesPerSecond * 3 / 2, 1.0] as CFArray)
        VTCompressionSessionPrepareToEncodeFrames(s)
        session = s
    }

    public func setBitrate(_ bps: Int) {
        guard let session, bps != settings.bitrateBps else { return }
        settings.bitrateBps = bps
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_AverageBitRate, value: bps as CFNumber)
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_DataRateLimits, value: [bps / 8 * 3 / 2, 1.0] as CFArray)
    }

    /// Asynchronous encode; `completion` runs on a VideoToolbox thread.
    public func encode(_ pixelBuffer: CVPixelBuffer, forceKeyframe: Bool, tag: UInt64, completion: @escaping @Sendable (Result<Output, EncoderError>) -> Void) {
        guard let session else { return completion(.failure(.session(kVTInvalidSessionErr))) }
        frameIndex += 1
        let pts = CMTime(value: frameIndex, timescale: CMTimeScale(settings.fps))
        let props: CFDictionary? = forceKeyframe ? [kVTEncodeFrameOptionKey_ForceKeyFrame: kCFBooleanTrue!] as CFDictionary : nil
        let t0 = MonoClock.nowMicros()
        let status = VTCompressionSessionEncodeFrame(session, imageBuffer: pixelBuffer, presentationTimeStamp: pts,
                                                     duration: .invalid, frameProperties: props, infoFlagsOut: nil) { status, _, sb in
            guard status == noErr, let sb, let out = VideoEncoder.extract(sb, encodeMicros: MonoClock.nowMicros() - t0, tag: tag) else {
                completion(.failure(.encode(status)))
                return
            }
            completion(.success(out))
        }
        if status != noErr { completion(.failure(.encode(status))) }
    }

    static func extract(_ sb: CMSampleBuffer, encodeMicros: UInt64, tag: UInt64) -> Output? {
        guard let block = CMSampleBufferGetDataBuffer(sb), let fd = CMSampleBufferGetFormatDescription(sb) else { return nil }
        let length = CMBlockBufferGetDataLength(block)
        var data = Data(count: length)
        let status = data.withUnsafeMutableBytes { CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: length, destination: $0.baseAddress!) }
        guard status == noErr else { return nil }
        var notSync = false
        if let attachments = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[CFString: Any]],
           let first = attachments.first {
            notSync = (first[kCMSampleAttachmentKey_NotSync] as? Bool) ?? false
        }
        return Output(data: data, parameterSets: parameterSets(fd), randomAccess: !notSync, encodeMicros: encodeMicros, tag: tag)
    }

    static func parameterSets(_ fd: CMFormatDescription) -> [[UInt8]] {
        let isHEVC = CMFormatDescriptionGetMediaSubType(fd) == kCMVideoCodecType_HEVC
        var count = 0
        var nalLen: Int32 = 0
        _ = isHEVC
            ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fd, parameterSetIndex: 0, parameterSetPointerOut: nil, parameterSetSizeOut: nil, parameterSetCountOut: &count, nalUnitHeaderLengthOut: &nalLen)
            : CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, parameterSetIndex: 0, parameterSetPointerOut: nil, parameterSetSizeOut: nil, parameterSetCountOut: &count, nalUnitHeaderLengthOut: &nalLen)
        var sets: [[UInt8]] = []
        for i in 0..<count {
            var ptr: UnsafePointer<UInt8>?
            var size = 0
            let st = isHEVC
                ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fd, parameterSetIndex: i, parameterSetPointerOut: &ptr, parameterSetSizeOut: &size, parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil)
                : CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, parameterSetIndex: i, parameterSetPointerOut: &ptr, parameterSetSizeOut: &size, parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil)
            if st == noErr, let ptr { sets.append(Array(UnsafeBufferPointer(start: ptr, count: size))) }
        }
        return sets
    }
}

/// BGRA -> NV12 on the GPU with the shared colour-contract kernel (plan §4.6 vendor-neutral preprocessing).
public final class NV12Preprocessor: @unchecked Sendable {
    public let pipeline: ColorPipeline
    public let width: Int
    public let height: Int
    private var pool: CVPixelBufferPool?

    public init(pipeline: ColorPipeline, width: Int, height: Int) {
        self.pipeline = pipeline
        self.width = width
        self.height = height
        let attrs: [CFString: Any] = [
            kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
            kCVPixelBufferWidthKey: width, kCVPixelBufferHeightKey: height,
            kCVPixelBufferIOSurfacePropertiesKey: [CFString: Any](),
            kCVPixelBufferMetalCompatibilityKey: true,
        ]
        CVPixelBufferPoolCreate(nil, [kCVPixelBufferPoolMinimumBufferCountKey: 4] as CFDictionary, attrs as CFDictionary, &pool)
    }

    /// Full-frame conversion (plan §4.1 P0 rule: full-frame copies, no partial updates). Blocks until the
    /// GPU finishes (~0.3 ms at 4K on Apple silicon) so the encoder never sees a half-written surface.
    public func convert(_ bgra: CVPixelBuffer) throws -> CVPixelBuffer {
        guard let pool else { throw ColorPipeline.PipelineError.unsupportedPixelFormat(0) }
        var out: CVPixelBuffer?
        CVPixelBufferPoolCreatePixelBuffer(nil, pool, &out)
        guard let out, let cb = pipeline.queue.makeCommandBuffer() else { throw ColorPipeline.PipelineError.noDevice }
        CVBufferSetAttachment(out, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, .shouldPropagate)
        CVBufferSetAttachment(out, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_sRGB, .shouldPropagate)
        CVBufferSetAttachment(out, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, .shouldPropagate)
        CVBufferSetAttachment(out, kCVImageBufferChromaLocationTopFieldKey, kCVImageBufferChromaLocation_Left, .shouldPropagate)
        let keep = try pipeline.encodeBGRAToNV12(source: bgra, destination: out, color: .sdrReference, commandBuffer: cb)
        cb.commit()
        cb.waitUntilCompleted()
        withExtendedLifetime(keep) {}
        return out
    }
}
#endif

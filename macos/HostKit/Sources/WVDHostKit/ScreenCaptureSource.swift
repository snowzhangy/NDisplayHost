#if os(macOS)
import AppKit
import CoreAudioTypes
import CoreMedia
import Foundation
import ScreenCaptureKit
import WVDAppleCore

/// Captures one existing display with ScreenCaptureKit (README: "Capture | ScreenCaptureKit (SCStream)").
///
/// The cursor is excluded from the video (`showsCursor = false`) and sent out of band (plan §6.2), so on
/// macOS the pointer state is always "separate" and there is never a double cursor.
public final class ScreenCaptureSource: NSObject, FrameSource, SCStreamOutput, SCStreamDelegate, @unchecked Sendable {
    public struct DisplayInfo: Sendable, Hashable, Identifiable {
        public var displayID: CGDirectDisplayID
        public var name: String
        public var pixelWidth: Int
        public var pixelHeight: Int
        public var refreshHz: Int
        public var id: CGDirectDisplayID { displayID }
    }

    public let name: String
    public let width: Int
    public let height: Int
    public let refreshHz: Int
    public let displayID: CGDirectDisplayID
    public var desktopFrame: CGRect { CGDisplayBounds(displayID) }

    private let fps: Int
    private var stream: SCStream?
    private let sampleQueue = DispatchQueue(label: "wvd.sck", qos: .userInteractive)
    private let audioQueue = DispatchQueue(label: "wvd.sck.audio", qos: .userInteractive)
    private let onAudio = Locked<(@Sendable (AudioChunk) -> Void)?>(nil)
    private var onFrame: (@Sendable (CapturedFrame) -> Void)?
    private let onStopped = Locked<(@Sendable (FrameSourceStop) -> Void)?>(nil)
    private let cursorCache = Locked<(image: CursorImage?, hash: Int)>((nil, 0))
    private var cursorTimer: Timer?

    public static func displays() async throws -> [DisplayInfo] {
        let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
        return content.displays.map { d in
            let mode = CGDisplayCopyDisplayMode(d.displayID)
            let screen = NSScreen.screens.first { ($0.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value == d.displayID }
            return DisplayInfo(displayID: d.displayID, name: screen?.localizedName ?? "Display \(d.displayID)",
                               pixelWidth: mode?.pixelWidth ?? d.width, pixelHeight: mode?.pixelHeight ?? d.height,
                               refreshHz: Int((mode?.refreshRate ?? 60) > 0 ? mode!.refreshRate : 60))
        }
    }

    public init(display: DisplayInfo, fps: Int = 60) {
        self.displayID = display.displayID
        self.name = display.name
        self.width = display.pixelWidth & ~1
        self.height = display.pixelHeight & ~1
        self.refreshHz = display.refreshHz
        self.fps = fps
        super.init()
    }

    public func start(frames: @escaping @Sendable (CapturedFrame) -> Void, stopped: @escaping @Sendable (FrameSourceStop) -> Void) async throws {
        onFrame = frames
        onStopped.withLock { $0 = stopped }
        let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
        guard let display = content.displays.first(where: { $0.displayID == displayID }) else {
            throw NSError(domain: "WVD", code: 1, userInfo: [NSLocalizedDescriptionKey: "Display not found"])
        }
        let config = SCStreamConfiguration()
        config.width = width
        config.height = height
        config.pixelFormat = kCVPixelFormatType_32BGRA
        config.colorSpaceName = CGColorSpace.sRGB      // plan §6.7: capture interpreted as sRGB
        config.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(fps))
        config.queueDepth = 3
        config.showsCursor = false
        // System audio for the headset (48 kHz stereo); our own process is excluded so nothing echoes.
        config.capturesAudio = true
        config.sampleRate = 48_000
        config.channelCount = 2
        config.excludesCurrentProcessAudio = true
        config.scalesToFit = false
        let filter = SCContentFilter(display: display, excludingWindows: [])
        let stream = SCStream(filter: filter, configuration: config, delegate: self)
        try stream.addStreamOutput(self, type: .screen, sampleHandlerQueue: sampleQueue)
        try stream.addStreamOutput(self, type: .audio, sampleHandlerQueue: audioQueue)
        try await stream.startCapture()
        self.stream = stream
        await MainActor.run { self.startCursorShapePolling() }
    }

    public func stop() async {
        onStopped.withLock { $0 = nil }  // an intentional stop is not a failure
        onAudio.withLock { $0 = nil }
        try? await stream?.stopCapture()
        stream = nil
        await MainActor.run {
            cursorTimer?.invalidate()
            cursorTimer = nil
        }
    }

    public func setAudioHandler(_ handler: (@Sendable (AudioChunk) -> Void)?) { onAudio.withLock { $0 = handler } }

    /// SCK audio arrives as Float32, usually one buffer per channel; convert to interleaved Int16.
    private func handleAudio(_ sb: CMSampleBuffer) {
        guard let handler = onAudio.snapshot, sb.isValid,
              let desc = CMSampleBufferGetFormatDescription(sb),
              let asbd = CMAudioFormatDescriptionGetStreamBasicDescription(desc)?.pointee,
              asbd.mFormatID == kAudioFormatLinearPCM, asbd.mFormatFlags & kAudioFormatFlagIsFloat != 0,
              asbd.mBitsPerChannel == 32, asbd.mChannelsPerFrame > 0 else { return }
        let channels = Int(asbd.mChannelsPerFrame)
        let frames = CMSampleBufferGetNumSamples(sb)
        guard frames > 0 else { return }
        var blockBuffer: CMBlockBuffer?
        var listSize = 0
        CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sb, bufferListSizeNeededOut: &listSize, bufferListOut: nil,
            bufferListSize: 0, blockBufferAllocator: nil, blockBufferMemoryAllocator: nil, flags: 0, blockBufferOut: nil)
        guard listSize > 0 else { return }
        let raw = UnsafeMutableRawPointer.allocate(byteCount: listSize, alignment: MemoryLayout<AudioBufferList>.alignment)
        defer { raw.deallocate() }
        let abl = raw.bindMemory(to: AudioBufferList.self, capacity: 1)
        guard CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sb, bufferListSizeNeededOut: nil, bufferListOut: abl,
            bufferListSize: listSize, blockBufferAllocator: nil, blockBufferMemoryAllocator: nil,
            flags: kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, blockBufferOut: &blockBuffer) == noErr else { return }
        let buffers = UnsafeMutableAudioBufferListPointer(abl)
        let planar = asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved != 0
        var out = [Int16](repeating: 0, count: frames * channels)
        @inline(__always) func s16(_ v: Float) -> Int16 { Int16((v.isFinite ? min(max(v, -1), 1) : 0) * 32767) }
        if planar {
            for c in 0..<min(channels, buffers.count) {
                guard let p = buffers[c].mData?.assumingMemoryBound(to: Float.self) else { continue }
                let n = min(frames, Int(buffers[c].mDataByteSize) / 4)
                for f in 0..<n { out[f * channels + c] = s16(p[f]) }
            }
        } else if let p = buffers.first?.mData?.assumingMemoryBound(to: Float.self) {
            let n = min(frames * channels, Int(buffers[0].mDataByteSize) / 4)
            for i in 0..<n { out[i] = s16(p[i]) }
        }
        handler(AudioChunk(sampleRate: UInt32(asbd.mSampleRate.rounded()), channels: channels, samples: out,
                           hostTimeMicros: MonoClock.nowMicros()))
    }

    public func stream(_ stream: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of type: SCStreamOutputType) {
        if type == .audio { handleAudio(sb); return }
        guard type == .screen, sb.isValid,
              let attachments = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
              let statusRaw = attachments.first?[.status] as? Int, let status = SCFrameStatus(rawValue: statusRaw),
              status == .complete, let pb = CMSampleBufferGetImageBuffer(sb) else { return }
        let pts = CMSampleBufferGetPresentationTimeStamp(sb)
        let micros = pts.isValid ? UInt64(max(0, CMTimeGetSeconds(pts)) * 1_000_000) : MonoClock.nowMicros()
        onFrame?(CapturedFrame(pixelBuffer: pb, captureTimeMicros: micros, changed: true))
    }

    /// SCK stopped on its own (display disconnected, permission revoked, user ended sharing in the menu bar,
    /// capture daemon crash). Report it so the session never keeps a frozen desktop with live input.
    public func stream(_ stream: SCStream, didStopWithError error: Error) {
        self.stream = nil
        let ns = error as NSError
        let stop: FrameSourceStop = ns.domain == SCStreamErrorDomain && ns.code == SCStreamError.Code.userStopped.rawValue
            ? .userStopped : .failed(error.localizedDescription)
        let cb = onStopped.withLock { c -> (@Sendable (FrameSourceStop) -> Void)? in defer { c = nil }; return c }
        cb?(stop)
    }

    // MARK: Cursor

    public func cursor() -> CursorSnapshot? {
        guard let loc = CGEvent(source: nil)?.location else { return nil }
        let bounds = CGDisplayBounds(displayID)
        let scale = Double(width) / max(bounds.width, 1)
        let inside = bounds.contains(loc)
        return CursorSnapshot(visible: inside, x: Float((loc.x - bounds.minX) * scale), y: Float((loc.y - bounds.minY) * scale),
                              shapeHash: cursorCache.snapshot.hash)
    }

    public func cursorImage() -> CursorImage? { cursorCache.snapshot.image }

    @MainActor
    private func startCursorShapePolling() {
        pollCursorShape()
        cursorTimer = Timer.scheduledTimer(withTimeInterval: 1.0 / 30.0, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.pollCursorShape() }
        }
    }

    @MainActor
    private func pollCursorShape() {
        guard let cursor = NSCursor.currentSystem else { return }
        let bounds = CGDisplayBounds(displayID)
        let scale = Double(width) / max(bounds.width, 1)
        let image = cursor.image
        let pw = max(1, Int((image.size.width * scale).rounded())), ph = max(1, Int((image.size.height * scale).rounded()))
        guard pw <= 256, ph <= 256 else { return }
        var rect = CGRect(x: 0, y: 0, width: image.size.width, height: image.size.height)
        guard let cg = image.cgImage(forProposedRect: &rect, context: nil, hints: nil) else { return }
        var rgba = [UInt8](repeating: 0, count: pw * ph * 4)
        let ok = rgba.withUnsafeMutableBytes { raw -> Bool in
            guard let ctx = CGContext(data: raw.baseAddress, width: pw, height: ph, bitsPerComponent: 8, bytesPerRow: pw * 4,
                                      space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return false }
            ctx.interpolationQuality = .high
            ctx.draw(cg, in: CGRect(x: 0, y: 0, width: pw, height: ph))
            return true
        }
        guard ok else { return }
        var hasher = Hasher()
        hasher.combine(rgba)
        hasher.combine(cursor.hotSpot.x)
        hasher.combine(cursor.hotSpot.y)
        let h = hasher.finalize()
        guard h != cursorCache.snapshot.hash else { return }
        let img = CursorImage(width: pw, height: ph, hotX: Int(cursor.hotSpot.x * scale), hotY: Int(cursor.hotSpot.y * scale), rgba: rgba)
        cursorCache.withLock { $0 = (img, h) }
    }
}
#endif

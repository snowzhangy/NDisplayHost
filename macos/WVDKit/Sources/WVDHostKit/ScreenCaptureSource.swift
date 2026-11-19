#if os(macOS)
import AppKit
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
        config.capturesAudio = false
        config.scalesToFit = false
        let filter = SCContentFilter(display: display, excludingWindows: [])
        let stream = SCStream(filter: filter, configuration: config, delegate: self)
        try stream.addStreamOutput(self, type: .screen, sampleHandlerQueue: sampleQueue)
        try await stream.startCapture()
        self.stream = stream
        await MainActor.run { self.startCursorShapePolling() }
    }

    public func stop() async {
        onStopped.withLock { $0 = nil }  // an intentional stop is not a failure
        try? await stream?.stopCapture()
        stream = nil
        await MainActor.run {
            cursorTimer?.invalidate()
            cursorTimer = nil
        }
    }

    public func stream(_ stream: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of type: SCStreamOutputType) {
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

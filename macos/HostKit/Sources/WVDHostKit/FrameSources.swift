#if os(macOS)
import CoreGraphics
import CoreMedia
import CoreVideo
import Foundation
import Metal
import WVDAppleCore

/// One captured desktop image (BGRA8, IOSurface-backed, interpreted as sRGB).
public struct CapturedFrame: @unchecked Sendable {
    public var pixelBuffer: CVPixelBuffer
    public var captureTimeMicros: UInt64
    /// False when the source reports no content change (static desktop).
    public var changed: Bool
}

/// Cursor as the capture source sees it (plan §6.2: the macOS host always uses the separate state).
public struct CursorSnapshot: Sendable, Equatable {
    public var visible: Bool
    public var x: Float            // hotspot, monitor pixels
    public var y: Float
    public var shapeHash: Int
}

public struct CursorImage: @unchecked Sendable {
    public var width: Int
    public var height: Int
    public var hotX: Int
    public var hotY: Int
    public var rgba: [UInt8]       // premultiplied RGBA8, top row first
}

/// Why a running source stopped delivering frames.
public enum FrameSourceStop: Sendable, Equatable {
    /// The user ended sharing through the system (e.g. the macOS screen-sharing menu).
    case userStopped
    case failed(String)
}

/// Captured system audio: interleaved signed 16-bit PCM.
public struct AudioChunk: Sendable {
    public var sampleRate: UInt32
    public var channels: Int
    public var samples: [Int16]
    public var hostTimeMicros: UInt64
    public init(sampleRate: UInt32, channels: Int, samples: [Int16], hostTimeMicros: UInt64) {
        self.sampleRate = sampleRate; self.channels = channels; self.samples = samples; self.hostTimeMicros = hostTimeMicros
    }
}

public protocol FrameSource: AnyObject, Sendable {
    var name: String { get }
    var width: Int { get }
    var height: Int { get }
    var refreshHz: Int { get }
    /// Points -> pixels and desktop origin, for input injection.
    var desktopFrame: CGRect { get }
    /// `stopped` fires at most once if capture ends after a successful start (not after `stop()`).
    func start(frames: @escaping @Sendable (CapturedFrame) -> Void, stopped: @escaping @Sendable (FrameSourceStop) -> Void) async throws
    func stop() async
    func cursor() -> CursorSnapshot?
    func cursorImage() -> CursorImage?
    /// System audio while the source runs (nil stops delivery). Sources without audio ignore it.
    func setAudioHandler(_ handler: (@Sendable (AudioChunk) -> Void)?)
}

extension FrameSource {
    public func setAudioHandler(_ handler: (@Sendable (AudioChunk) -> Void)?) {}
}

/// GPU-generated test desktop (README bring-up step 2: headless CI without capture permissions).
///
/// Content: moving gradient, 1-px line grids and checkers (text-like detail), saturated bars, a moving
/// box, and a 24-bit binary frame counter in the top-left (plan §8 "per-frame binary counter").
/// `staticContent` stops the animation so refinement and idle behaviour can be tested.
public final class SyntheticSource: FrameSource, @unchecked Sendable {
    public let name = "Synthetic test pattern"
    public let width: Int
    public let height: Int
    public let refreshHz: Int
    public var desktopFrame: CGRect { CGRect(x: 0, y: 0, width: width, height: height) }
    public let staticContent: Locked<Bool>

    private let device: MTLDevice
    private let queue: MTLCommandQueue
    private let pipeline: MTLComputePipelineState
    private var pool: CVPixelBufferPool?
    private var cache: CVMetalTextureCache?
    private var timer: DispatchSourceTimer?
    private let timerQueue = DispatchQueue(label: "wvd.synthetic", qos: .userInteractive)
    private var frameIndex: UInt32 = 0
    private var startedAt: UInt64 = 0
    private let cursorPhase = Locked<Double>(0)

    public static let shader = #"""
    #include <metal_stdlib>
    using namespace metal;
    kernel void wvd_synthetic(texture2d<float, access::write> out [[texture(0)]],
                              constant uint& frame [[buffer(0)]],
                              uint2 gid [[thread_position_in_grid]]) {
        uint W = out.get_width(), H = out.get_height();
        if (gid.x >= W || gid.y >= H) { return; }
        float t = float(frame) / 60.0f;
        float u = float(gid.x) / float(W), v = float(gid.y) / float(H);
        float3 c = float3(0.15f + 0.1f * sin(u * 6.0f + t), 0.18f + 0.1f * v, 0.22f + 0.1f * cos(v * 5.0f - t));
        // Text-like detail: 1-px grid and checker blocks.
        if (v > 0.2f && v < 0.45f) {
            uint x = gid.x, y = gid.y;
            float s = (u < 0.25f) ? float(x & 1) : (u < 0.5f) ? float(y & 1) : (u < 0.75f) ? float((x ^ y) & 1) : float(((x >> 1) ^ (y >> 1)) & 1);
            c = mix(float3(0.97f), float3(0.08f), s);
        }
        // Saturated bars.
        if (v > 0.5f && v < 0.6f) {
            uint bar = uint(u * 8.0f);
            c = float3((bar >> 2) & 1, (bar >> 1) & 1, bar & 1);
        }
        // Moving box.
        float bx = fract(t * 0.1f) * 0.8f + 0.1f;
        if (fabs(u - bx) < 0.04f && v > 0.65f && v < 0.85f) { c = float3(0.95f, 0.6f, 0.1f); }
        // 24-bit frame counter: 24 cells of 16x16 px, white = 1.
        uint cell = gid.x / 16;
        if (gid.y < 16 && cell < 24) {
            c = ((frame >> (23 - cell)) & 1) ? float3(1.0f) : float3(0.0f);
        }
        out.write(float4(c, 1.0f), gid);
    }
    """#

    public init(width: Int = 1920, height: Int = 1080, refreshHz: Int = 60, staticContent: Bool = false) throws {
        guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else {
            throw ColorPipeline.PipelineError.noDevice
        }
        self.width = width
        self.height = height
        self.refreshHz = refreshHz
        self.device = device
        self.queue = queue
        self.staticContent = Locked(staticContent)
        let lib = try device.makeLibrary(source: Self.shader, options: nil)
        pipeline = try device.makeComputePipelineState(function: lib.makeFunction(name: "wvd_synthetic")!)
        let attrs: [CFString: Any] = [
            kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA,
            kCVPixelBufferWidthKey: width, kCVPixelBufferHeightKey: height,
            kCVPixelBufferMetalCompatibilityKey: true, kCVPixelBufferIOSurfacePropertiesKey: [CFString: Any](),
        ]
        CVPixelBufferPoolCreate(nil, [kCVPixelBufferPoolMinimumBufferCountKey: 4] as CFDictionary, attrs as CFDictionary, &pool)
        CVMetalTextureCacheCreate(nil, nil, device, nil, &cache)
    }

    private let onStopped = Locked<(@Sendable (FrameSourceStop) -> Void)?>(nil)

    /// Test hook: behave like a capture backend that failed (or was stopped by the user) mid-stream.
    public func simulateStop(_ stop: FrameSourceStop) {
        timerQueue.sync {
            timer?.cancel()
            timer = nil
        }
        let cb = onStopped.withLock { c -> (@Sendable (FrameSourceStop) -> Void)? in defer { c = nil }; return c }
        cb?(stop)
    }

    public func start(frames: @escaping @Sendable (CapturedFrame) -> Void, stopped: @escaping @Sendable (FrameSourceStop) -> Void) async throws {
        onStopped.withLock { $0 = stopped }
        let t = DispatchSource.makeTimerSource(flags: .strict, queue: timerQueue)
        let interval = 1.0 / Double(refreshHz)
        t.schedule(deadline: .now(), repeating: interval, leeway: .microseconds(200))
        var lastStatic = false
        t.setEventHandler { [weak self] in
            guard let self else { return }
            let isStatic = self.staticContent.snapshot
            if isStatic && lastStatic { return }  // nothing changed: a real desktop would deliver no frame either
            lastStatic = isStatic
            if let f = self.render() { frames(f) }
        }
        timerQueue.sync { timer = t }
        startedAt = MonoClock.nowMicros()
        t.resume()
    }

    public func stop() async {
        onStopped.withLock { $0 = nil }
        timerQueue.sync {
            timer?.cancel()
            timer = nil
        }
        setAudioHandler(nil)
    }

    // MARK: Audio (test tone)

    private var audioTimer: DispatchSourceTimer?
    private let audioPhase = Locked<Double>(0)

    /// A 440 Hz tone on the left channel and 660 Hz on the right, in 10 ms chunks at 48 kHz.
    public func setAudioHandler(_ handler: (@Sendable (AudioChunk) -> Void)?) {
        timerQueue.sync {
            audioTimer?.cancel()
            audioTimer = nil
            guard let handler else { return }
            let t = DispatchSource.makeTimerSource(flags: .strict, queue: timerQueue)
            t.schedule(deadline: .now(), repeating: .milliseconds(10), leeway: .microseconds(500))
            let phase = audioPhase
            t.setEventHandler {
                let frames = 480
                var samples = [Int16](repeating: 0, count: frames * 2)
                phase.withLock { p in
                    for f in 0..<frames {
                        let t = p + Double(f) / 48_000
                        samples[f * 2] = Int16(sin(2 * .pi * 440 * t) * 8000)
                        samples[f * 2 + 1] = Int16(sin(2 * .pi * 660 * t) * 8000)
                    }
                    p += Double(frames) / 48_000
                }
                handler(AudioChunk(sampleRate: 48_000, channels: 2, samples: samples, hostTimeMicros: MonoClock.nowMicros()))
            }
            t.resume()
            audioTimer = t
        }
    }

    /// Frame index rendered into the counter of the most recent frame.
    public var renderedFrames: UInt32 { timerQueue.sync { frameIndex } }

    /// Renders one frame synchronously (also used by benchmarks).
    public func render() -> CapturedFrame? {
        guard let pool, let cache else { return nil }
        var pb: CVPixelBuffer?
        guard CVPixelBufferPoolCreatePixelBuffer(nil, pool, &pb) == kCVReturnSuccess, let pb else { return nil }
        var cvTex: CVMetalTexture?
        CVMetalTextureCacheCreateTextureFromImage(nil, cache, pb, nil, .bgra8Unorm, width, height, 0, &cvTex)
        guard let cvTex, let tex = CVMetalTextureGetTexture(cvTex), let cb = queue.makeCommandBuffer(),
              let enc = cb.makeComputeCommandEncoder() else { return nil }
        frameIndex &+= 1
        var f = frameIndex
        enc.setComputePipelineState(pipeline)
        enc.setTexture(tex, index: 0)
        enc.setBytes(&f, length: 4, index: 0)
        let w = pipeline.threadExecutionWidth
        enc.dispatchThreads(MTLSize(width: width, height: height, depth: 1),
                            threadsPerThreadgroup: MTLSize(width: w, height: pipeline.maxTotalThreadsPerThreadgroup / w, depth: 1))
        enc.endEncoding()
        cb.commit()
        cb.waitUntilCompleted()
        withExtendedLifetime(cvTex) {}
        cursorPhase.withLock { $0 += 0.02 }
        return CapturedFrame(pixelBuffer: pb, captureTimeMicros: MonoClock.nowMicros(), changed: true)
    }

    public func cursor() -> CursorSnapshot? {
        let p = cursorPhase.snapshot
        return CursorSnapshot(visible: true, x: Float(Double(width) * (0.5 + 0.3 * cos(p))),
                              y: Float(Double(height) * (0.5 + 0.3 * sin(p))), shapeHash: 1)
    }

    public func cursorImage() -> CursorImage? { CursorImage.arrow(scale: 2) }
}

extension CursorImage {
    /// A plain arrow (used by the synthetic source and as a fallback shape).
    public static func arrow(scale: Int) -> CursorImage {
        let rows = [
            "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ",
            "X.....X     ", "X......X    ", "X.......X   ", "X........X  ", "X.....XXXXX ", "X..X..X     ",
            "X.X X..X    ", "XX  X..X    ", "X    X..X   ", "     X..X   ", "      XX    ",
        ]
        let w = 12 * scale, h = rows.count * scale
        var rgba = [UInt8](repeating: 0, count: w * h * 4)
        for (y, row) in rows.enumerated() {
            for (x, ch) in row.enumerated() {
                let px: [UInt8]? = ch == "X" ? [0, 0, 0, 255] : ch == "." ? [255, 255, 255, 255] : nil
                guard let px else { continue }
                for sy in 0..<scale { for sx in 0..<scale {
                    let i = ((y * scale + sy) * w + (x * scale + sx)) * 4
                    rgba.replaceSubrange(i..<i + 4, with: px)
                } }
            }
        }
        return CursorImage(width: w, height: h, hotX: 0, hotY: 0, rgba: rgba)
    }
}
#endif

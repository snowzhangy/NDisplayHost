import CoreVideo
import Foundation
import Metal
import Network
import WVDAppleCore
import WVDHostKit

// Headless macOS host and benchmarks.
//
//   wvd-host serve [--display N | --synthetic] [--fps 60] [--mbps 150] [--port 47800] [--allow-input]
//   wvd-host bench [--width 3840 --height 2160] [--mbps 80] [--h264] [--low-latency-rc] [--quality]
//
// `serve` opens the pairing window at start and prints the SAS; type "y" + return to accept.

let args = Array(CommandLine.arguments.dropFirst())
func value(_ name: String) -> String? {
    guard let i = args.firstIndex(of: name), i + 1 < args.count else { return nil }
    return args[i + 1]
}
func flag(_ name: String) -> Bool { args.contains(name) }

func serve() async throws {
    let identity = try HostIdentity.loadOrCreate()
    var config = HostServer.Config()
    config.port = UInt16(value("--port") ?? "") ?? WVDNet.defaultPort
    config.fps = Int(value("--fps") ?? "") ?? 60
    config.maxBitrateBps = (Int(value("--mbps") ?? "") ?? 150) * 1_000_000
    config.startBitrateBps = min(config.startBitrateBps, config.maxBitrateBps)
    config.autoConsent = flag("--allow-input")
    let injector: InputInjector = flag("--allow-input") ? CGEventInjector() : RecordingInjector()
    if flag("--allow-input") && !injector.isTrusted { CGEventInjector.promptForAccessibility() }

    let displayIndex = Int(value("--display") ?? "")
    let displays = (try? await ScreenCaptureSource.displays()) ?? []
    let fps = config.fps
    let server: HostServer
    var events = HostServer.Events()
    events.status = { print("status:", $0) }
    events.log = { print("log:", $0) }
    events.pairingRequest = { r in
        guard let r else { return }
        print("\nPairing request from \(r.deviceName) (\(r.deviceModel)). Code: \(r.sas.prefix(3)) \(r.sas.suffix(3))")
        print("Accept? [y/N] ", terminator: "")
        fflush(stdout)
    }
    events.session = { s in
        guard let s else { return print("session: none") }
        print(String(format: "session: %@ %dx%d %.0f fps %.0f/%.0f Mbit/s enc p95 %.1f ms rtt %.1f ms e2e p95 %.0f ms loss %.2f%% input %@",
                     s.deviceName, s.width, s.height, s.fps, s.bitrateMbps, s.targetMbps, s.encodeP95Millis, s.rttMillis,
                     s.e2eP95Millis, s.lossPercent, "\(s.input)"))
    }
    server = try HostServer(identity: identity, devices: .standard(), injector: injector, config: config, events: events) {
        if let displayIndex, displays.indices.contains(displayIndex) {
            return ScreenCaptureSource(display: displays[displayIndex], fps: fps)
        }
        return try SyntheticSource(width: 2560, height: 1440, refreshHz: fps)
    }
    try server.start()
    server.openPairingWindow(seconds: 600)
    print("WVD host \(identity.fingerprint) on port \(config.port). Displays:")
    for (i, d) in displays.enumerated() { print("  [\(i)] \(d.name) \(d.pixelWidth)x\(d.pixelHeight)") }
    print("Source: \(displayIndex.map { "display \($0)" } ?? "synthetic test pattern"). Pairing open for 10 minutes.")
    // Read the terminal off the main thread: ScreenCaptureKit cursor polling runs on the main actor.
    Thread.detachNewThread {
        while let line = readLine() {
            server.respondToPairing(accept: line.lowercased().hasPrefix("y"))
        }
    }
    while true { try await Task.sleep(for: .seconds(3600)) }
}

enum BenchError: Error { case invalidSettings, readPixels }

/// Squared difference on 8-bit luma against the NV12 that went into the encoder.
///
/// Speed on its own cannot tell an encoder that is fast from one that is fast because it dropped
/// detail, and this is the number that lets a hardware tier be compared with VideoToolbox at the
/// same resolution and bitrate instead of by eye. It is measured on the encoder's own input, which
/// is what it is being judged on, rather than on the source's RGB.
func lumaError(reference: CVPixelBuffer, decoded: CVPixelBuffer) throws -> (squared: UInt64, pixels: UInt64) {
    guard CVPixelBufferLockBaseAddress(reference, .readOnly) == kCVReturnSuccess else { throw BenchError.readPixels }
    defer { CVPixelBufferUnlockBaseAddress(reference, .readOnly) }
    guard CVPixelBufferLockBaseAddress(decoded, .readOnly) == kCVReturnSuccess else { throw BenchError.readPixels }
    defer { CVPixelBufferUnlockBaseAddress(decoded, .readOnly) }
    let width = CVPixelBufferGetWidthOfPlane(reference, 0)
    let height = CVPixelBufferGetHeightOfPlane(reference, 0)
    guard width == CVPixelBufferGetWidthOfPlane(decoded, 0), height == CVPixelBufferGetHeightOfPlane(decoded, 0),
          let referenceBase = CVPixelBufferGetBaseAddressOfPlane(reference, 0),
          let decodedBase = CVPixelBufferGetBaseAddressOfPlane(decoded, 0) else { throw BenchError.readPixels }
    let referenceStride = CVPixelBufferGetBytesPerRowOfPlane(reference, 0)
    let decodedStride = CVPixelBufferGetBytesPerRowOfPlane(decoded, 0)
    var squared: UInt64 = 0
    for y in 0..<height {
        let a = referenceBase.advanced(by: y * referenceStride).assumingMemoryBound(to: UInt8.self)
        let b = decodedBase.advanced(by: y * decodedStride).assumingMemoryBound(to: UInt8.self)
        var row = 0
        for x in 0..<width {
            let delta = Int(a[x]) - Int(b[x])
            row += delta * delta
        }
        squared += UInt64(row)
    }
    return (squared, UInt64(width) * UInt64(height))
}

/// Per-stage costs on this Mac (plan §8 metric definitions; encode = submit -> last byte).
///
/// A frame the encoder or decoder drops is skipped rather than fatal: a benchmark that stops on the
/// first hiccup reports the timings of the run up to it and calls that the machine.
func bench() throws {
    guard let w = Int(value("--width") ?? "3840"), (2...8192).contains(w), w.isMultiple(of: 2),
          let h = Int(value("--height") ?? "2160"), (2...8192).contains(h), h.isMultiple(of: 2),
          let mbps = Int(value("--mbps") ?? "80"), (1...1000).contains(mbps) else {
        throw BenchError.invalidSettings
    }
    let fps = 60
    let frameCount = 120
    let measureQuality = flag("--quality")
    let pipeline = try ColorPipeline()
    let source = try SyntheticSource(width: w, height: h)
    let pre = NV12Preprocessor(pipeline: pipeline, width: w, height: h)
    let codec: CodecKind = flag("--h264") ? .h264 : .hevc
    let encoder = try VideoEncoder(settings: .init(width: w, height: h, fps: fps, bitrateBps: mbps * 1_000_000, codec: codec,
                                                   lowLatencyRateControl: flag("--low-latency-rc")))
    print("VideoToolbox low-latency rate control: \(encoder.lowLatencyRateControl)")
    var convert = PercentileWindow(), encode = PercentileWindow(), fec = PercentileWindow(), decode = PercentileWindow(), rgb = PercentileWindow()
    var sizes: [Int] = []
    let done = DispatchSemaphore(value: 0)
    let outputs = Locked<[VideoEncoder.Output]>([])
    var decoder: VideoDecoder?
    var squaredError: UInt64 = 0
    var pixels: UInt64 = 0
    let rgbDesc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm_srgb, width: w, height: h, mipmapped: true)
    rgbDesc.usage = [.shaderRead, .shaderWrite, .renderTarget, .pixelFormatView]
    rgbDesc.storageMode = .private
    let rgbTex = pipeline.device.makeTexture(descriptor: rgbDesc)!
    let rgbView = rgbTex.makeTextureView(pixelFormat: .rgba8Unorm)!
    for i in 0..<frameCount {
        guard let frame = source.render() else { continue }
        let t0 = MonoClock.nowMicros()
        let nv12 = try pre.convert(frame.pixelBuffer)
        convert.add(micros: MonoClock.nowMicros() - t0)
        encoder.encode(nv12, forceKeyframe: i == 0 || i % 30 == 0, tag: UInt64(i)) { r in
            if case .success(let o) = r { outputs.withLock { $0.append(o) } }
            done.signal()
        }
        done.wait()
        guard let out = outputs.withLock({ $0.popLast() }) else { continue }
        encode.add(micros: out.encodeMicros)
        sizes.append(out.data.count)
        let t1 = MonoClock.nowMicros()
        let desc = FrameDescriptor(monitorID: 1, sessionGen: 1, layoutGen: 1, codecGen: 1, picSeq: UInt32(i + 1), contentVer: 1,
                                   randomAccess: out.randomAccess, refinement: false, captureTimeMicros: 0, deadlineBudgetMillis: 100)
        let pf = out.data.withUnsafeBytes { Packetizer.packetize($0, descriptor: desc, config: PacketizerConfig()) }
        pf.computeParity()
        fec.add(micros: MonoClock.nowMicros() - t1)
        if decoder == nil {
            var c = CodecConfig()
            c.codec = codec; c.codecGen = 1; c.width = UInt32(w); c.height = UInt32(h)
            c.parameterSets = out.parameterSets; c.color = .sdrReference
            decoder = try VideoDecoder(config: c)
        }
        let au = out.data.withUnsafeBytes { AUBuffer(copying: $0) }
        let t2 = MonoClock.nowMicros()
        guard case .success(let pb) = decoder!.decode(au) else { continue }
        decode.add(micros: MonoClock.nowMicros() - t2)
        let t3 = MonoClock.nowMicros()
        let cb = pipeline.queue.makeCommandBuffer()!
        let keep = try pipeline.encodeNV12ToRGBA(pb, color: .sdrReference, into: rgbView, linearOut: false, commandBuffer: cb)
        pipeline.encodeMipmaps(rgbTex, commandBuffer: cb)
        cb.commit()
        cb.waitUntilCompleted()
        withExtendedLifetime(keep) {}
        rgb.add(micros: MonoClock.nowMicros() - t3)
        if measureQuality, let error = try? lumaError(reference: nv12, decoded: pb) {
            squaredError += error.squared
            pixels += error.pixels
        }
    }
    func line(_ name: String, _ p: PercentileWindow) {
        print(String(format: "%-28@ p50 %6.2f ms   p95 %6.2f ms   p99 %6.2f ms", name as NSString,
                     Double(p.p50) / 1000, Double(p.p95) / 1000, Double(p.p99) / 1000))
    }
    let codecName = codec == .h264 ? "H.264" : "HEVC"
    print("\(codecName), \(w)x\(h) @ \(fps) fps, \(frameCount) frames, IDR every 30, \(mbps) Mbit/s target")
    line("BGRA->NV12 (GPU)", convert)
    line("Encode (VT)", encode)
    line("packetize + RS parity", fec)
    line("Decode (VT)", decode)
    line("NV12->RGB + mips (GPU)", rgb)
    let sorted = sizes.sorted()
    if !sorted.isEmpty {
        print("AU size p50 \(sorted[sorted.count / 2] / 1024) KiB, max \(sorted.last! / 1024) KiB")
        // What the rate controller actually produced, which is the number a hardware encoder is
        // compared at: two encoders at the same *target* are not at the same bitrate.
        let encodedMbps = Double(sizes.reduce(0, +)) * 8 * Double(fps) / Double(frameCount) / 1e6
        print(String(format: "Encoded video %.2f Mbit/s at source cadence", encodedMbps))
    }
    if measureQuality {
        guard pixels > 0, squaredError > 0 else { print("Luma PSNR: no frames measured") ; return }
        let mse = Double(squaredError) / Double(pixels)
        print(String(format: "Luma PSNR %.2f dB", 10 * log10(255.0 * 255.0 / mse)))
    }
}

do {
    switch args.first {
    case "bench": try bench()
    case "serve", nil: try await serve()
    default:
        print("usage: wvd-host serve [--display N] [--fps 60] [--mbps 150] [--port 47800] [--allow-input] | bench [--width W --height H --mbps N --h264 --low-latency-rc --quality]")
    }
} catch {
    print("error: \(error)")
    exit(1)
}

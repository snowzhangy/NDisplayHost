import CoreVideo
import Foundation
import Metal
import Network
import WVDAppleCore
import WVDHostKit

// Headless macOS host and benchmarks.
//
//   wvd-host serve [--display N | --synthetic] [--fps 60] [--mbps 150] [--port 47800] [--allow-input]
//   wvd-host bench [--width 3840 --height 2160] [--h264] [--low-latency-rc]
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

/// Per-stage costs on this Mac (plan §8 metric definitions; encode = submit -> last byte).
func bench() throws {
    let w = Int(value("--width") ?? "") ?? 3840, h = Int(value("--height") ?? "") ?? 2160
    let pipeline = try ColorPipeline()
    let source = try SyntheticSource(width: w, height: h)
    let pre = NV12Preprocessor(pipeline: pipeline, width: w, height: h)
    let codec: CodecKind = flag("--h264") ? .h264 : .hevc
    let encoder = try VideoEncoder(settings: .init(width: w, height: h, fps: 60, bitrateBps: 80_000_000, codec: codec,
                                                   lowLatencyRateControl: flag("--low-latency-rc")))
    print("VideoToolbox low-latency rate control: \(encoder.lowLatencyRateControl)")
    var convert = PercentileWindow(), encode = PercentileWindow(), fec = PercentileWindow(), decode = PercentileWindow(), rgb = PercentileWindow()
    var sizes: [Int] = []
    let done = DispatchSemaphore(value: 0)
    let outputs = Locked<[VideoEncoder.Output]>([])
    var decoder: VideoDecoder?
    let rgbDesc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm_srgb, width: w, height: h, mipmapped: true)
    rgbDesc.usage = [.shaderRead, .shaderWrite, .renderTarget, .pixelFormatView]
    rgbDesc.storageMode = .private
    let rgbTex = pipeline.device.makeTexture(descriptor: rgbDesc)!
    let rgbView = rgbTex.makeTextureView(pixelFormat: .rgba8Unorm)!
    for i in 0..<120 {
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
    }
    func line(_ name: String, _ p: PercentileWindow) {
        print(String(format: "%-28@ p50 %6.2f ms   p95 %6.2f ms   p99 %6.2f ms", name as NSString,
                     Double(p.p50) / 1000, Double(p.p95) / 1000, Double(p.p99) / 1000))
    }
    print("\(w)x\(h), 120 frames, IDR every 30")
    line("BGRA->NV12 (GPU)", convert)
    line("HEVC encode (VT)", encode)
    line("packetize + RS parity", fec)
    line("HEVC decode (VT)", decode)
    line("NV12->RGB + mips (GPU)", rgb)
    let sorted = sizes.sorted()
    if !sorted.isEmpty {
        print("AU size p50 \(sorted[sorted.count / 2] / 1024) KiB, max \(sorted.last! / 1024) KiB")
    }
}

do {
    switch args.first {
    case "bench": try bench()
    case "serve", nil: try await serve()
    default:
        print("usage: wvd-host serve [--display N] [--fps 60] [--mbps 150] [--port 47800] [--allow-input] | bench [--width W --height H]")
    }
} catch {
    print("error: \(error)")
    exit(1)
}

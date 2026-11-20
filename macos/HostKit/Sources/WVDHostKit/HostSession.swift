#if os(macOS)
import CoreGraphics
import CoreVideo
import Foundation
import Network
import WVDAppleCore

/// One client connection: flows, authentication/pairing, consent, and the media pipeline
///   capture -> newest-wins mailbox -> NV12 (GPU) -> VideoToolbox -> packetize + FEC -> pacer -> QUIC datagrams
/// with generations, IDR-on-demand recovery, NACK retransmission, static refinement and ABR (plan §6).
public final class HostSession: @unchecked Sendable {
    public struct Snapshot: Sendable, Equatable {
        public var deviceName: String
        public var deviceModel: String
        public var authenticated: Bool
        public var input: InputArmState
        public var visible: Bool
        public var width: Int
        public var height: Int
        public var fps: Double
        public var bitrateMbps: Double
        public var targetMbps: Double
        public var encodeP95Millis: Double
        public var rttMillis: Double
        public var e2eP95Millis: Double
        public var lossPercent: Double
        public var thermal: ThermalLevel
    }

    private enum AuthState { case awaitingHello, pairing, authenticated, closed }

    weak var server: HostServer?
    let sessionGen: UInt32
    private let group: NWConnectionGroup
    private let queue: DispatchQueue
    private let encodeQueue = DispatchQueue(label: "wvd.host.encode", qos: .userInteractive)
    private let packetQueue = DispatchQueue(label: "wvd.host.packet", qos: .userInteractive)

    // Flows and TLS.
    private var control: EnvelopeStream?
    private var inputStream: EnvelopeStream?
    private let datagramFlow = Locked<NWConnection?>(nil)
    private var sasExporter: [UInt8] = []
    private var authExporter: [UInt8] = []
    private var pairExporter: [UInt8] = []

    // Auth.
    private var auth: AuthState = .awaitingHello
    private var hello: Hello?
    private(set) var deviceID: String?
    private var clientConfirmedPairing: Bool?
    private var hostConfirmedPairing: Bool?
    private(set) var isClosed = false

    // Input (plan §4.4 / §4.7).
    private var inputState: InputArmState = .disarmed
    private var inputGen: UInt32 = 1
    private var mediaFlowBound = false
    /// True once this session injected an event. The injector is shared by every connection, so only the
    /// session that may own held keys/buttons releases them (a rejected second connection must not).
    private var mayHoldInput = false

    // Media pipeline.
    private var source: FrameSource?
    private var monitor = MonitorInfo()
    private let layoutGen: UInt32 = 1
    private let mailbox = NewestWinsMailbox<CapturedFrame>()
    private var preprocessor: NV12Preprocessor?
    private var encoder: VideoEncoder?
    private var pacer: Pacer?
    private var packetConfig = PacketizerConfig()
    private let pipe = Locked(PipeState())
    private let history = Locked<[UInt32: PacketizedFrame]>([:])
    private var timers: [DispatchSourceTimer] = []

    // Cursor.
    private var cursorShapeHash = 0
    private var cursorShapeVer: UInt32 = 0
    private var cursorSeq: UInt32 = 0
    /// The client asked for audio (StreamSettings.audio); nothing is sent otherwise.
    private var audioEnabled = false
    private var audioSeq: UInt32 = 0
    private var lastCursor: CursorSnapshot?
    private var lastCursorSentAt: UInt64 = 0

    // Stats / ABR.
    private var lastClientStats = ClientStats()
    private var cleanWindows = 0
    private var thermal: ThermalLevel = .nominal
    private var maxBitrate: Int

    /// Encode-side state shared by the encode and packet queues.
    struct PipeState {
        var streaming = false
        var visible = true
        var encoderBusy = false
        var pendingIDR = true
        var lastIDRPicSeq: UInt32 = 0
        var lastIDRAt: UInt64 = 0
        var retained: UncheckedSendable<CVPixelBuffer>?
        var retainedCapture: UInt64 = 0
        var lastChangeAt: UInt64 = 0
        var refinementsLeft = 0
        var refinementScheduled = false
        var contentVer: UInt32 = 0
        var picSeq: UInt32 = 0
        var codecGen: UInt32 = 0
        var parameterSets: [[UInt8]] = []
        var bitrate = 0
        var fecRatio = 0.10
        var captured = 0
        var encoded = 0
        var bytesSent = 0
        var retransmitted = 0
        var idrCount = 0
        var encodeMicros = PercentileWindow()
        var windowStart = MonoClock.nowMicros()
        var fps: Double = 0
        var mbps: Double = 0
    }

    init(group: NWConnectionGroup, server: HostServer, sessionGen: UInt32) {
        self.group = group
        self.server = server
        self.queue = server.queue
        self.sessionGen = sessionGen
        self.maxBitrate = server.config.maxBitrateBps
    }

    func start() {
        group.newConnectionHandler = { [weak self] conn in self?.classify(conn) }
        group.stateUpdateHandler = { [weak self] state in
            switch state {
            case .failed(let e): self?.close(reason: WVDNet.describe(e))
            case .cancelled: self?.close(reason: nil)
            default: break
            }
        }
        // Required before start (see QUICClientConnection.start); datagrams are read per flow.
        group.setReceiveHandler(maximumMessageSize: 65_535, rejectOversizedMessages: true) { [weak self] _, content, _ in
            if let content { self?.onDatagram(content) }
        }
        group.start(queue: queue)
        // A connection that never authenticates is dropped (plan §4.7: fail closed).
        queue.asyncAfter(deadline: .now() + 30) { [weak self] in
            guard let self, self.auth == .awaitingHello else { return }
            self.close(reason: "handshake timeout")
        }
    }

    // MARK: Flow classification

    /// Network.framework surfaces streams and the datagram flow alike; the first bytes tell them apart
    /// ("WVD1" stream preamble vs FlowHello datagram).
    private func classify(_ conn: NWConnection) {
        conn.stateUpdateHandler = { [weak self, weak conn] state in
            guard let self, let conn, case .ready = state else { return }
            self.readFirst(conn, have: Data())
        }
        conn.start(queue: queue)
    }

    private func readFirst(_ conn: NWConnection, have: Data) {
        conn.receive(minimumIncompleteLength: 1, maximumLength: 65_536) { [weak self] data, _, isComplete, error in
            guard let self, error == nil else { return }
            var buf = have
            if let data { buf.append(data) }
            // A stream may deliver its preamble in pieces; a datagram always arrives whole.
            if buf.count < 4 && Array(buf) == Array(StreamFraming.preamble.prefix(buf.count)) && !isComplete {
                self.readFirst(conn, have: buf)
            } else {
                self.firstBytes(buf, on: conn)
            }
        }
    }

    private func firstBytes(_ data: Data, on conn: NWConnection) {
        if data.count >= 4, Array(data.prefix(4)) == StreamFraming.preamble {
            let stream = EnvelopeStream(connection: conn, queue: queue, expectPreamble: false)
            let first = Locked(true)
            stream.start(initial: data.dropFirst(4), onEnvelope: { [weak self] env in
                guard let self else { return }
                if first.withLock({ f in defer { f = false }; return f }) {
                    switch env {
                    case .hello: self.adoptControl(stream, conn)
                    case .inputStreamOpen(let o) where o.sessionGen == self.sessionGen && self.auth == .authenticated:
                        self.inputStream = stream
                        return
                    default:
                        stream.cancel()
                        return
                    }
                }
                if self.inputStream === stream { self.onInputEnvelope(env) } else { self.onControlEnvelope(env) }
            }, onClose: { [weak self] _ in
                guard let self else { return }
                if self.control === stream { self.close(reason: "control stream closed") }
                if self.inputStream === stream {
                    self.inputStream = nil
                    self.releaseOwnedInput()
                }
            })
        } else if let fh = data.withUnsafeBytes({ FlowHello.parse($0) }) {
            guard fh.sessionGen == sessionGen, auth == .authenticated else { return }
            let isNew = datagramFlow.withLock { f -> Bool in
                if f === conn { return false }
                f = conn
                return true
            }
            if isNew { receiveDatagrams(conn) }
            if !mediaFlowBound {
                mediaFlowBound = true
                sendSessionState()
                // Anything paced before the flow existed was dropped: start the stream with a fresh IDR.
                let started = pipe.withLock { p -> Bool in
                    if p.picSeq > 0 { p.pendingIDR = true }
                    return p.picSeq > 0
                }
                if started { encodeQueue.async { [weak self] in self?.pump() } }
            }
        } else {
            conn.cancel()
        }
    }

    private func adoptControl(_ stream: EnvelopeStream, _ conn: NWConnection) {
        guard control == nil else { stream.cancel(); return }
        control = stream  // our preamble goes out with the first frame we send
        if let md = conn.metadata(definition: NWProtocolQUIC.definition) as? NWProtocolQUIC.Metadata {
            sasExporter = WVDCrypto.exporter(md.securityProtocolMetadata, label: WVDCrypto.sasLabel) ?? []
            authExporter = WVDCrypto.exporter(md.securityProtocolMetadata, label: WVDCrypto.authLabel) ?? []
            pairExporter = WVDCrypto.exporter(md.securityProtocolMetadata, label: WVDCrypto.pairLabel) ?? []
        }
    }

    private func receiveDatagrams(_ conn: NWConnection) {
        conn.receiveMessage { [weak self] data, _, _, error in
            guard let self else { return }
            if let data { self.onDatagram(data) }
            if error == nil { self.receiveDatagrams(conn) }
        }
    }

    private func send(_ env: Envelope) { control?.send(env) }

    // MARK: Control

    private func onControlEnvelope(_ env: Envelope) {
        switch env {
        case .hello(let h): handleHello(h)
        case .pairingConfirm(let c): handlePairingConfirm(c)
        case .ping(let p):
            let recv = MonoClock.nowMicros()
            send(.pong(Pong(clientSendMicros: p.clientSendMicros, hostRecvMicros: recv, hostSendMicros: MonoClock.nowMicros(), seq: p.seq)))
        case .recoveryRequest(let r) where auth == .authenticated: handleRecovery(r)
        case .clientStats(let s) where auth == .authenticated: handleClientStats(s)
        case .visibility(let v) where auth == .authenticated:
            guard v.monitorIDs.isEmpty || v.monitorIDs.contains(monitor.monitorID) else { break }
            let visible = v.visible
            let changed = pipe.withLock { p -> Bool in
                defer { p.visible = visible }
                // Only a hidden -> visible transition needs a fresh IDR; replayed states must not cost one.
                if visible && !p.visible { p.pendingIDR = true }
                return p.visible != visible
            }
            // A hidden monitor takes no input: release, and fence events already in flight (new input_gen).
            if !visible && changed { releaseAndBumpInput() }
            encodeQueue.async { [weak self] in self?.pump() }
        case .thermal(let t) where auth == .authenticated:
            thermal = t.state
            applyBitrateCaps()
        case .streamSettings(let s) where auth == .authenticated:
            audioEnabled = s.audio
            if s.maxBitrateKbps > 0 {
                maxBitrate = min(Int(s.maxBitrateKbps) * 1000, server?.config.maxBitrateBps ?? Int.max)
                applyBitrateCaps()
            }
        case .inputRelease where auth == .authenticated:
            releaseAndBumpInput()
        case .goodbye(let g): close(reason: g.reason.isEmpty ? "client left" : g.reason)
        default: break
        }
    }

    private func handleHello(_ h: Hello) {
        guard auth == .awaitingHello, let server else { return }
        hello = h
        var ack = HelloAck()
        ack.hostID = server.identity.hostID
        ack.hostName = server.config.hostName
        ack.hostOS = "macOS \(ProcessInfo.processInfo.operatingSystemVersionString)"
        ack.sessionGen = sessionGen
        ack.hostTimeMicros = MonoClock.nowMicros()

        guard h.protocolVersion == wvdProtocolVersion, !authExporter.isEmpty else {
            ack.auth = .rejected
            send(.helloAck(ack))
            return close(reason: "protocol mismatch")
        }
        if !h.authSignature.isEmpty {
            guard let device = server.devices.device(h.deviceID), WVDCrypto.equal(device.publicKeyX963, h.devicePublicKey),
                  WVDCrypto.verify(signature: h.authSignature,
                                   transcript: WVDCrypto.authTranscript(exporter: authExporter, hostCertSHA256: server.identity.certSHA256,
                                                                        devicePublicKey: h.devicePublicKey, deviceID: h.deviceID),
                                   publicKeyX963: h.devicePublicKey) else {
                ack.auth = .rejected
                send(.helloAck(ack))
                return close(reason: "authentication failed")
            }
            guard server.claimActive(self) else {
                ack.auth = .busy
                send(.helloAck(ack))
                return close(reason: "busy")
            }
            ack.auth = .ok
            send(.helloAck(ack))
            deviceID = h.deviceID
            server.devices.touch(h.deviceID)
            authenticated()
        } else if h.requestPairing {
            if server.pairingLockedOut {
                ack.auth = .lockedOut
            } else if !server.pairingAllowed {
                ack.auth = .pairingRequired
            } else if !server.claimActive(self) {
                ack.auth = .busy
            } else {
                ack.auth = .pairingStarted
            }
            send(.helloAck(ack))
            guard ack.auth == .pairingStarted else { return close(reason: "pairing not allowed") }
            auth = .pairing
            send(.pairingChallenge(PairingChallenge(sasDigits: 6, timeoutMillis: 120_000)))
            let sas = WVDCrypto.shortAuthenticationString(sasExporter: sasExporter, hostCertSHA256: server.identity.certSHA256,
                                                          devicePublicKey: h.devicePublicKey)
            if server.config.autoAcceptPairing {
                hostConfirmedPairing = true
            } else {
                server.events.pairingRequest(HostServer.PairingRequest(deviceName: h.deviceName, deviceModel: h.deviceModel, sas: sas))
            }
            queue.asyncAfter(deadline: .now() + 120) { [weak self] in
                guard let self, self.auth == .pairing else { return }
                self.finishPairing(success: false, reason: "Pairing timed out")
            }
        } else {
            ack.auth = .rejected
            send(.helloAck(ack))
            close(reason: "no credentials")
        }
    }

    private func handlePairingConfirm(_ c: PairingConfirm) {
        guard auth == .pairing, let h = hello, let server else { return }
        if !c.accepted {
            return finishPairing(success: false, reason: "Cancelled on the headset")
        }
        let transcript = WVDCrypto.pairTranscript(exporter: pairExporter, hostCertSHA256: server.identity.certSHA256,
                                                  devicePublicKey: h.devicePublicKey, deviceID: h.deviceID)
        guard WVDCrypto.verify(signature: c.deviceSignature, transcript: transcript, publicKeyX963: h.devicePublicKey) else {
            return finishPairing(success: false, reason: "Invalid device signature")
        }
        clientConfirmedPairing = true
        maybeCompletePairing()
    }

    func hostPairingDecision(_ accept: Bool) {
        guard auth == .pairing else { return }
        if !accept { return finishPairing(success: false, reason: "Rejected on the host") }
        hostConfirmedPairing = true
        maybeCompletePairing()
    }

    private func maybeCompletePairing() {
        if clientConfirmedPairing == true && hostConfirmedPairing == true { finishPairing(success: true, reason: "") }
    }

    private func finishPairing(success: Bool, reason: String) {
        guard let server, let h = hello else { return }
        server.events.pairingRequest(nil)
        if success {
            server.devices.save(PairedDevice(deviceID: h.deviceID, name: h.deviceName, model: h.deviceModel,
                                             publicKeyX963: h.devicePublicKey, pairedAt: Date(), lastSeen: Date()))
            server.closePairingWindow()
            send(.pairingResult(PairingResult(success: true, hostCertSHA256: server.identity.certSHA256)))
            deviceID = h.deviceID
            authenticated()
        } else {
            server.notePairingFailure()
            send(.pairingResult(PairingResult(success: false, reason: reason)))
            queue.asyncAfter(deadline: .now() + 0.2) { [weak self] in self?.close(reason: reason) }
            auth = .closed
        }
    }

    private func authenticated() {
        guard let server else { return }
        auth = .authenticated
        server.events.log("Authenticated \(hello?.deviceName ?? "device")")
        if server.config.autoConsent {
            inputState = .armed
        } else {
            inputState = .consentPending
            server.events.consentRequest(hello?.deviceName ?? "Vision Pro")
        }
        sendSessionState()
        startMedia()
        publishSnapshot()
    }

    // MARK: Consent and input

    func consentDecision(_ allow: Bool) {
        guard auth == .authenticated, inputState == .consentPending else { return }
        server?.events.consentRequest(nil)
        inputState = allow ? .armed : .denied
        inputGen &+= 1
        sendSessionState()
        publishSnapshot()
    }

    func disarm(reason: String) {
        guard auth == .authenticated else { return }
        // Kill switch: release unconditionally (only the active, authenticated session can reach here).
        server?.injector.releaseAll()
        mayHoldInput = false
        inputState = .disarmed
        inputGen &+= 1
        sendSessionState(message: reason)
        publishSnapshot()
    }

    private func releaseAndBumpInput() {
        releaseOwnedInput()
        inputGen &+= 1
        sendSessionState()
    }

    /// Releases held keys/buttons only if this session injected any (the injector is shared).
    private func releaseOwnedInput() {
        guard mayHoldInput else { return }
        mayHoldInput = false
        server?.injector.releaseAll()
    }

    private func sendSessionState(message: String = "") {
        send(.sessionState(SessionState(input: inputState, inputGen: inputGen, workstationLocked: false,
                                        message: message, mediaFlowBound: mediaFlowBound)))
    }

    private func onInputEnvelope(_ env: Envelope) {
        guard auth == .authenticated, let server else { return }
        switch env {
        case .input(let batch):
            // Events from older generations, while disarmed, or for a hidden monitor are rejected (plan §4.4).
            guard inputState == .armed, batch.inputGen == inputGen, pipe.snapshot.visible, source != nil else { return }
            mayHoldInput = true
            let px = CGSize(width: Int(monitor.width), height: Int(monitor.height))
            let frame = source?.desktopFrame ?? .zero
            for e in batch.events {
                guard let kind = e.kind else { continue }
                switch kind {
                case .move(let m) where m.monitorID != monitor.monitorID || m.layoutGen != layoutGen: continue
                case .button(let b) where b.monitorID != monitor.monitorID || b.layoutGen != layoutGen: continue
                case .text(let t) where t.text.count > 4096: continue
                default: break
                }
                server.injector.apply(kind, monitorPixels: px, desktopFrame: frame)
            }
        case .inputRelease:
            releaseAndBumpInput()
        default:
            break
        }
    }

    // MARK: Media pipeline

    private func startMedia() {
        guard let server else { return }
        let cfg = server.config
        let datagramFlow = self.datagramFlow
        let lossRate = cfg.simulatedLossRate, burst = max(1, cfg.simulatedLossBurst)
        let dropping = Locked(0)
        pacer = Pacer(config: cfg.pacer) { batch in
            guard let flow = datagramFlow.snapshot else { return }
            flow.batch {
                for d in batch {
                    if lossRate > 0 {
                        let drop = dropping.withLock { n -> Bool in
                            if n > 0 { n -= 1; return true }
                            if Double.random(in: 0..<1) < lossRate / Double(burst) { n = burst - 1; return true }
                            return false
                        }
                        if drop { continue }
                    }
                    flow.send(content: d, completion: .idempotent)
                }
            }
        }
        pipe.withLock { p in
            p.bitrate = cfg.startBitrateBps
            p.streaming = true
        }
        let factory = server.sourceFactory
        Task { [weak self] in
            let result: Result<FrameSource, Error>
            do { result = .success(try await factory()) } catch { result = .failure(error) }
            guard let self else { return }
            self.queue.async { [weak self] in
                switch result {
                case .success(let src): self?.sourceReady(src)
                case .failure(let e): self?.close(reason: "capture failed: \(e.localizedDescription)")
                }
            }
        }
    }

    private func sourceReady(_ src: FrameSource) {
        guard !isClosed, let server else { return }
        source = src
        let fps = server.config.fps
        monitor = MonitorInfo(monitorID: 1, name: src.name, width: UInt32(src.width), height: UInt32(src.height),
                              refreshMilliHz: UInt32(fps * 1000), ppi: 110, desktopX: Int32(src.desktopFrame.minX),
                              desktopY: Int32(src.desktopFrame.minY), primary: true)
        send(.monitorLayout(MonitorLayout(layoutGen: layoutGen, monitors: [monitor])))
        let usable = Int(hello?.maxDatagramSize ?? 1200)
        packetConfig = PacketizerConfig.forDatagramSize(min(max(usable, 1200), 1400))
        do {
            preprocessor = NV12Preprocessor(pipeline: server.colorPipeline, width: src.width, height: src.height)
            encoder = try VideoEncoder(settings: .init(width: src.width, height: src.height, fps: fps,
                                                       bitrateBps: pipe.snapshot.bitrate))
        } catch {
            return close(reason: "encoder: \(error)")
        }
        let mailbox = self.mailbox
        let pipe = self.pipe
        let wake: @Sendable () -> Void = { [weak self] in
            guard let self else { return }
            self.encodeQueue.async { [weak self] in self?.pump() }
        }
        let stopped: @Sendable (FrameSourceStop) -> Void = { [weak self] stop in
            guard let self else { return }
            self.queue.async { [weak self] in self?.captureStopped(stop) }
        }
        Task { [weak self] in
            do {
                try await src.start(frames: { frame in
                    pipe.withLock { $0.captured += 1 }
                    if mailbox.put(frame) { wake() }
                }, stopped: stopped)
            } catch {
                guard let self else { return }
                self.queue.async { [weak self] in self?.close(reason: "capture failed: \(error.localizedDescription)") }
            }
        }
        src.setAudioHandler { [weak self] chunk in
            guard let self else { return }
            self.queue.async { [weak self] in self?.sendAudio(chunk) }
        }
        repeating(every: .milliseconds(4)) { [weak self] in self?.cursorTick() }
        repeating(every: .seconds(1)) { [weak self] in self?.statsTick() }
    }

    /// Capture ended underneath a live session (display gone, permission revoked, sharing stopped by the
    /// user). Never leave a frozen but controllable desktop: release input and end the session with a reason.
    /// A failure is retryable (the client reconnects and the new session restarts capture); the user stopping
    /// sharing is not.
    private func captureStopped(_ stop: FrameSourceStop) {
        guard !isClosed else { return }
        let reason: String
        switch stop {
        case .userStopped: reason = "Screen sharing ended on the host"
        case .failed(let why): reason = "capture stopped: \(why)"
        }
        releaseOwnedInput()
        inputState = .disarmed
        inputGen &+= 1
        sendSessionState(message: reason)
        close(reason: reason)
    }

    /// Encode loop (encodeQueue). One picture in the encoder at a time keeps latency minimal; the
    /// capture mailbox drops stale frames instead of queueing them (newest wins).
    private func pump() {
        guard let encoder, let preprocessor, let pacer else { return }
        let frameInterval = UInt64(1_000_000 / max(encoder.settings.fps, 1))
        let now = MonoClock.nowMicros()
        enum Work { case fresh(CapturedFrame), retained(CVPixelBuffer, refinement: Bool, capture: UInt64) }
        let cfg = server?.config
        let work: Work? = pipe.withLock { p in
            guard p.streaming, p.visible, !p.encoderBusy, pacer.backlogBytes < 2 << 20 else { return nil }
            if let f = mailbox.take() {
                p.contentVer &+= 1
                p.lastChangeAt = now
                p.refinementsLeft = cfg?.refinementCount ?? 0
                return .fresh(f)
            }
            if p.pendingIDR, let r = p.retained {
                // Only once the IDR may actually be sent; re-encoding the same picture as P-frames is waste.
                guard now - p.lastIDRAt >= (cfg?.minIDRIntervalMicros ?? 100_000) else { return nil }
                return .retained(r.value, refinement: false, capture: now)
            }
            if p.refinementsLeft > 0, let r = p.retained, now - p.lastChangeAt >= (cfg?.refinementDelayMicros ?? 150_000) {
                p.refinementsLeft -= 1
                return .retained(r.value, refinement: true, capture: now)
            }
            return nil
        }
        // Schedule the next refinement check while content is static.
        let refinementPending = pipe.withLock { p -> Bool in
            guard p.refinementsLeft > 0, !p.refinementScheduled else { return false }
            p.refinementScheduled = true
            return true
        }
        if refinementPending {
            let delay = Int(cfg?.refinementIntervalMicros ?? 60_000)
            encodeQueue.asyncAfter(deadline: .now() + .microseconds(delay)) { [weak self] in
                self?.pipe.withLock { $0.refinementScheduled = false }
                self?.pump()
            }
        }
        guard let work else {
            // An IDR is wanted but rate-limited: come back when it is allowed.
            let wait = pipe.withLock { p -> UInt64? in
                guard p.pendingIDR, p.retained != nil, !p.encoderBusy else { return nil }
                let minGap = cfg?.minIDRIntervalMicros ?? 100_000
                let elapsed = now - p.lastIDRAt
                return elapsed < minGap ? minGap - elapsed : nil
            }
            if let wait {
                encodeQueue.asyncAfter(deadline: .now() + .microseconds(Int(wait) + 500)) { [weak self] in self?.pump() }
            }
            return
        }

        let nv12: CVPixelBuffer
        var refinement = false
        var capture: UInt64
        switch work {
        case .fresh(let f):
            guard let converted = try? preprocessor.convert(f.pixelBuffer) else { return }
            nv12 = converted
            capture = f.captureTimeMicros
            pipe.withLock { $0.retained = UncheckedSendable(converted); $0.retainedCapture = f.captureTimeMicros }
        case .retained(let pb, let r, let c):
            nv12 = pb
            refinement = r
            capture = c
        }
        let (forceIDR, contentVer) = pipe.withLock { p -> (Bool, UInt32) in
            let force = p.pendingIDR && now - p.lastIDRAt >= (cfg?.minIDRIntervalMicros ?? 100_000)
            if force { p.pendingIDR = false }
            p.encoderBusy = true
            return (force, p.contentVer)
        }
        if refinement && forceIDR { refinement = false }
        let captureTime = min(capture, now)
        let isRefinement = refinement
        encoder.encode(nv12, forceKeyframe: forceIDR, tag: captureTime) { [weak self] result in
            guard let self else { return }
            switch result {
            case .success(let out):
                self.packetQueue.async {
                    self.ship(out, capture: captureTime, contentVer: contentVer, refinement: isRefinement, frameInterval: frameInterval)
                }
            case .failure:
                self.pipe.withLock { $0.pendingIDR = true }
            }
            self.pipe.withLock { $0.encoderBusy = false }
            self.encodeQueue.async { self.pump() }
        }
    }

    /// Packetize, pace and remember one encoded picture (packetQueue, strictly in encode order).
    private func ship(_ out: VideoEncoder.Output, capture: UInt64, contentVer: UInt32, refinement: Bool, frameInterval: UInt64) {
        guard let pacer, let server else { return }
        let codec = encoder?.settings.codec ?? .hevc
        let ra = out.randomAccess && out.data.withUnsafeBytes { Bitstream.isRandomAccess($0, codec: codec) }
        var newConfig: CodecConfig?
        let (picSeq, codecGen, bitrate, fec) = pipe.withLock { p -> (UInt32, UInt32, Int, Double) in
            if p.parameterSets != out.parameterSets && !out.parameterSets.isEmpty {
                // New parameter sets = new codec generation, announced reliably before its media.
                p.codecGen &+= 1
                p.parameterSets = out.parameterSets
                var c = CodecConfig()
                c.monitorID = monitor.monitorID
                c.codecGen = p.codecGen
                c.codec = codec
                c.width = monitor.width
                c.height = monitor.height
                c.parameterSets = out.parameterSets
                c.color = .sdrReference
                c.nalLengthSize = 4
                c.frameRateMilliHz = monitor.refreshMilliHz
                newConfig = c
            }
            p.picSeq &+= 1
            p.encoded += 1
            p.bytesSent += out.data.count
            p.encodeMicros.add(micros: out.encodeMicros)
            if ra {
                p.lastIDRPicSeq = p.picSeq
                p.lastIDRAt = MonoClock.nowMicros()
                p.idrCount += 1
            }
            return (p.picSeq, p.codecGen, p.bitrate, p.fecRatio)
        }
        if let newConfig { queue.async { [weak self] in self?.send(.codecConfig(newConfig)) } }
        guard codecGen > 0 else { return }

        let desc = FrameDescriptor(monitorID: UInt16(monitor.monitorID), sessionGen: sessionGen, layoutGen: layoutGen,
                                   codecGen: codecGen, picSeq: picSeq, contentVer: contentVer, randomAccess: ra,
                                   refinement: refinement, captureTimeMicros: capture,
                                   deadlineBudgetMillis: server.config.deadlineBudgetMillis)
        var cfg = packetConfig
        cfg.fecRatio = fec
        let frame = out.data.withUnsafeBytes { Packetizer.packetize($0, descriptor: desc, config: cfg) }
        pacer.enqueue(frame.interleavedData(), frameIntervalMicros: frameInterval, avgBitrateBps: Double(bitrate))
        frame.computeParity()
        pacer.enqueue(frame.interleavedParity(), frameIntervalMicros: frameInterval, avgBitrateBps: Double(bitrate))
        history.withLock { h in
            h[picSeq] = frame
            if h.count > 48, let oldest = h.keys.min() { h[oldest] = nil }
        }
    }

    private func onDatagram(_ data: Data) {
        guard let type = data.first else { return }
        switch type {
        case DatagramType.nack.rawValue:
            guard let nack = data.withUnsafeBytes({ NackDatagram.parse($0) }), let pacer, let server else { return }
            guard let frame = history.withLock({ $0[nack.picSeq] }) else { return }
            // Only worth resending while the picture can still make its deadline.
            let age = MonoClock.nowMicros() - frame.descriptor.captureTimeMicros
            guard age < UInt64(server.config.deadlineBudgetMillis) * 1000 else { return }
            let resend = frame.retransmission(of: nack.missing)
            pipe.withLock { $0.retransmitted += resend.count }
            pacer.enqueue(resend, frameIntervalMicros: 2_000, avgBitrateBps: Double(pipe.snapshot.bitrate))
        case DatagramType.flowHello.rawValue:
            break
        default:
            break
        }
    }

    private func handleRecovery(_ r: RecoveryRequest) {
        let now = MonoClock.nowMicros()
        let accepted = pipe.withLock { p -> Bool in
            // An IDR newer than the client's last good picture is already on its way.
            if p.lastIDRPicSeq > r.lastGoodPicSeq && now - p.lastIDRAt < 150_000 { return false }
            p.pendingIDR = true
            return true
        }
        if accepted { encodeQueue.async { [weak self] in self?.pump() } }
    }

    // MARK: ABR, FEC adaptation, thermal

    private func handleClientStats(_ s: ClientStats) {
        lastClientStats = s
        let expected = Double(s.shardsReceived + s.shardsMissing)
        let lossRate = expected > 0 ? Double(s.shardsMissing) / expected : 0
        var bitrate = pipe.snapshot.bitrate
        let minBR = server?.config.minBitrateBps ?? 8_000_000
        if s.framesLost > 0 || lossRate > 0.02 || s.framesLate > 2 {
            bitrate = max(minBR, Int(Double(bitrate) * 0.8))
            cleanWindows = 0
        } else {
            cleanWindows += 1
            if cleanWindows >= 3 { bitrate = Int(Double(bitrate) * 1.08) }
        }
        let fec = min(0.30, max(0.06, lossRate * 3 + 0.05))
        pipe.withLock { p in
            p.bitrate = bitrate
            p.fecRatio = fec
        }
        applyBitrateCaps()
    }

    private func applyBitrateCaps() {
        var cap = maxBitrate
        switch thermal {
        case .serious: cap = cap * 6 / 10
        case .critical: cap = cap * 4 / 10
        default: break
        }
        let br = pipe.withLock { p -> Int in
            p.bitrate = min(p.bitrate, cap)
            return p.bitrate
        }
        encodeQueue.async { [weak self] in self?.encoder?.setBitrate(br) }
    }

    // MARK: Cursor (plan §6.2: shape reliable on change, position datagrams on every move)

    private func cursorTick() {
        guard let source, auth == .authenticated, pipe.snapshot.visible, let flow = datagramFlow.snapshot else { return }
        guard let snap = source.cursor() else { return }
        if snap.shapeHash != cursorShapeHash, let img = source.cursorImage() {
            cursorShapeHash = snap.shapeHash
            cursorShapeVer &+= 1
            var shape = CursorShape()
            shape.monitorID = monitor.monitorID
            shape.shapeVer = cursorShapeVer
            shape.layoutGen = layoutGen
            shape.width = UInt32(img.width)
            shape.height = UInt32(img.height)
            shape.hotX = UInt32(img.hotX)
            shape.hotY = UInt32(img.hotY)
            shape.rgba = img.rgba
            shape.state = .separate
            send(.cursorShape(shape))
        }
        let now = MonoClock.nowMicros()
        let moved = lastCursor.map { $0.x != snap.x || $0.y != snap.y || $0.visible != snap.visible } ?? true
        guard moved || now - lastCursorSentAt > 100_000, cursorShapeVer > 0 else { return }
        lastCursor = snap
        lastCursorSentAt = now
        cursorSeq &+= 1
        let d = CursorDatagram(visible: snap.visible, monitorID: UInt16(monitor.monitorID), layoutGen: layoutGen,
                               shapeVer: cursorShapeVer, seq: cursorSeq, x: snap.x, y: snap.y, hostTimeMicros: now)
        flow.send(content: d.encoded(), completion: .idempotent)
    }

    // MARK: Audio (uncompressed PCM datagrams, about 5 ms each; see AudioPacket)

    private func sendAudio(_ chunk: AudioChunk) {
        guard audioEnabled, auth == .authenticated, !isClosed, let flow = datagramFlow.snapshot,
              chunk.channels > 0, chunk.sampleRate > 0 else { return }
        let usable = Int(hello?.maxDatagramSize ?? 1200)
        let fit = (min(max(usable, 1200), 1400) - AudioPacket.headerSize) / (chunk.channels * 2)
        let perPacket = max(1, min(Int(chunk.sampleRate) / 200, fit))
        let packets = AudioPacket.packetize(chunk.samples, channels: chunk.channels, sampleRate: chunk.sampleRate,
                                            framesPerPacket: perPacket, sessionGen: sessionGen, firstSeq: audioSeq,
                                            hostTimeMicros: chunk.hostTimeMicros)
        audioSeq &+= UInt32(packets.count)
        flow.batch {
            for p in packets { flow.send(content: p.encoded(), completion: .idempotent) }
        }
    }

    // MARK: Stats

    private func statsTick() {
        let now = MonoClock.nowMicros()
        var hs = HostStats()
        pipe.withLock { p in
            let window = Double(max(now - p.windowStart, 1)) / 1e6
            p.fps = Double(p.encoded) / window
            p.mbps = Double(p.bytesSent * 8) / window / 1e6
            hs.monitorID = monitor.monitorID
            hs.fpsCaptured = UInt32(Double(p.captured) / window)
            hs.fpsEncoded = UInt32(p.fps.rounded())
            hs.bitrateKbps = UInt32(p.mbps * 1000)
            hs.encodeMicrosP50 = p.encodeMicros.p50
            hs.encodeMicrosP95 = p.encodeMicros.p95
            hs.targetBitrateKbps = UInt32(p.bitrate / 1000)
            hs.retransmittedShards = UInt32(p.retransmitted)
            hs.idrCount = UInt32(p.idrCount)
            p.captured = 0; p.encoded = 0; p.bytesSent = 0; p.retransmitted = 0
            p.windowStart = now
        }
        hs.queues = [
            QueueDepth(name: "capture", depth: mailbox.hasValue ? 1 : 0, capacity: 1, peak: 1, dropped: mailbox.counters.superseded),
            QueueDepth(name: "pacer_bytes", depth: UInt32(pacer?.backlogBytes ?? 0), capacity: UInt32(server?.config.pacer.maxQueuedBytes ?? 0)),
        ]
        send(.hostStats(hs))
        publishSnapshot()
    }

    var snapshot: Snapshot {
        let p = pipe.snapshot
        let c = lastClientStats
        let expected = Double(c.shardsReceived + c.shardsMissing)
        return Snapshot(deviceName: hello?.deviceName ?? "", deviceModel: hello?.deviceModel ?? "",
                        authenticated: auth == .authenticated, input: inputState, visible: p.visible,
                        width: Int(monitor.width), height: Int(monitor.height), fps: p.fps, bitrateMbps: p.mbps,
                        targetMbps: Double(p.bitrate) / 1e6, encodeP95Millis: Double(p.encodeMicros.p95) / 1000,
                        rttMillis: Double(c.rttMicros) / 1000, e2eP95Millis: Double(c.e2eMicrosP95) / 1000,
                        lossPercent: expected > 0 ? Double(c.shardsMissing) / expected * 100 : 0, thermal: thermal)
    }

    private func publishSnapshot() {
        guard let server, server.activeSession === self else { return }
        server.events.session(snapshot)
    }

    private func repeating(every interval: DispatchTimeInterval, _ body: @escaping () -> Void) {
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + interval, repeating: interval, leeway: .milliseconds(1))
        t.setEventHandler(handler: body)
        t.resume()
        timers.append(t)
    }

    // MARK: Teardown

    func close(reason: String?) {
        let work: @Sendable () -> Void = { [self] in
            guard !isClosed else { return }
            isClosed = true
            auth = .closed
            if let reason { server?.events.log("Session closed: \(reason)") }
            releaseOwnedInput()
            timers.forEach { $0.cancel() }
            timers.removeAll()
            pipe.withLock { $0.streaming = false }
            pacer?.stop()
            if let reason { control?.send(.goodbye(Goodbye(reason: reason))) }
            let src = source
            Task { await src?.stop() }
            queue.asyncAfter(deadline: .now() + 0.1) { [group] in group.cancel() }
            server?.sessionClosed(self)
        }
        queue.async(execute: work)
    }
}
#endif

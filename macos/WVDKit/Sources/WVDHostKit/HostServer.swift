#if os(macOS)
import CoreGraphics
import Foundation
import Network
import WVDAppleCore

/// QUIC listener + Bonjour advertisement + session lifecycle, pairing window and consent (plan §4.2, §4.7).
public final class HostServer: @unchecked Sendable {
    public struct Config: Sendable {
        public var port: UInt16 = WVDNet.defaultPort
        public var hostName: String = Host.current().localizedName ?? "Mac"
        public var advertise = true
        public var fps = 60
        public var startBitrateBps = 60_000_000
        public var maxBitrateBps = 150_000_000
        public var minBitrateBps = 8_000_000
        public var deadlineBudgetMillis: UInt16 = 100
        /// Static content: refine `refinementCount` times, first after `refinementDelayMicros` (plan §6.4 T_q).
        public var refinementDelayMicros: UInt64 = 150_000
        public var refinementIntervalMicros: UInt64 = 60_000
        public var refinementCount = 4
        public var minIDRIntervalMicros: UInt64 = 100_000
        /// Tests only: skip the user prompts.
        public var autoAcceptPairing = false
        public var autoConsent = false
        public var pacer = Pacer.Config()
        /// Test hook: drop this fraction of outgoing media datagrams, in bursts of `simulatedLossBurst`.
        public var simulatedLossRate = 0.0
        public var simulatedLossBurst = 1
        public init() {}
    }

    public enum Status: Sendable, Equatable {
        case stopped
        case listening(port: UInt16)
        case failed(String)
    }

    public struct PairingRequest: Sendable, Equatable {
        public var deviceName: String
        public var deviceModel: String
        public var sas: String
    }

    public struct Events: Sendable {
        public var status: @Sendable (Status) -> Void = { _ in }
        public var session: @Sendable (HostSession.Snapshot?) -> Void = { _ in }
        public var pairingRequest: @Sendable (PairingRequest?) -> Void = { _ in }
        public var consentRequest: @Sendable (String?) -> Void = { _ in }
        public var log: @Sendable (String) -> Void = { _ in }
        public init() {}
    }

    public let identity: HostIdentity
    public let devices: PairedDeviceStore
    public let injector: InputInjector
    public let config: Config
    let events: Events
    let sourceFactory: @Sendable () async throws -> FrameSource
    let colorPipeline: ColorPipeline

    let queue = DispatchQueue(label: "wvd.host.server", qos: .userInteractive)
    private var listener: NWListener?
    private var sessions: [ObjectIdentifier: HostSession] = [:]
    private(set) var activeSession: HostSession?
    private var pairingWindowUntil: UInt64 = 0
    private var failedPairings: [UInt64] = []
    private var sessionGen: UInt32 = 0
    public private(set) var boundPort: UInt16?

    public init(identity: HostIdentity, devices: PairedDeviceStore, injector: InputInjector, config: Config = Config(),
                events: Events = Events(), sourceFactory: @escaping @Sendable () async throws -> FrameSource) throws {
        self.identity = identity
        self.devices = devices
        self.injector = injector
        self.config = config
        self.events = events
        self.sourceFactory = sourceFactory
        self.colorPipeline = try ColorPipeline()
    }

    public func start() throws {
        let quic = WVDNet.quicOptions()
        guard let secIdentity = sec_identity_create(identity.identity) else { throw HostIdentity.IdentityError.certificate }
        sec_protocol_options_set_local_identity(quic.securityProtocolOptions, secIdentity)
        let params = WVDNet.parameters(quic)
        params.allowLocalEndpointReuse = true  // restart on the same port right after a stop
        let port = config.port == 0 ? NWEndpoint.Port.any : NWEndpoint.Port(rawValue: config.port) ?? .any
        let listener = try NWListener(using: params, on: port)
        if config.advertise {
            let txt = NWTXTRecord(["id": identity.hostID, "v": "\(wvdProtocolVersion)"])
            listener.service = NWListener.Service(name: config.hostName, type: WVDNet.bonjourType, txtRecord: txt)
        }
        listener.newConnectionGroupHandler = { [weak self] group in self?.accept(group) }
        listener.stateUpdateHandler = { [weak self] state in
            guard let self else { return }
            switch state {
            case .ready:
                self.boundPort = listener.port?.rawValue
                self.events.status(.listening(port: listener.port?.rawValue ?? 0))
            case .failed(let e):
                self.events.status(.failed(WVDNet.describe(e)))
            case .cancelled:
                self.events.status(.stopped)
                let waiters = self.stopWaiters
                self.stopWaiters.removeAll()
                waiters.forEach { $0() }
            default: break
            }
        }
        listener.start(queue: queue)
        self.listener = listener
    }

    /// Waits until the listener has a port (tests).
    public func waitUntilListening(timeout: TimeInterval = 5) -> UInt16? {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if let p = queue.sync(execute: { boundPort }), p != 0 { return p }
            Thread.sleep(forTimeInterval: 0.01)
        }
        return nil
    }

    private var stopWaiters: [@Sendable () -> Void] = []

    /// Stops listening and ends every session. `completion` runs once the port is released.
    public func stop(completion: (@Sendable () -> Void)? = nil) {
        queue.async { [self] in
            for s in sessions.values { s.close(reason: "host stopped") }
            sessions.removeAll()
            activeSession = nil
            guard let l = listener else { completion?(); return }
            if let completion { stopWaiters.append(completion) }
            l.cancel()
            listener = nil
            boundPort = nil
        }
    }

    private func accept(_ group: NWConnectionGroup) {
        sessionGen &+= 1
        let session = HostSession(group: group, server: self, sessionGen: sessionGen)
        sessions[ObjectIdentifier(session)] = session
        session.start()
    }

    // MARK: Called by sessions (on `queue`)

    func sessionClosed(_ s: HostSession) {
        sessions[ObjectIdentifier(s)] = nil
        if activeSession === s {
            activeSession = nil
            events.session(nil)
            events.consentRequest(nil)
        }
    }

    func claimActive(_ s: HostSession) -> Bool {
        if let a = activeSession, a !== s, !a.isClosed { return false }
        activeSession = s
        return true
    }

    var pairingAllowed: Bool { MonoClock.nowMicros() < pairingWindowUntil || config.autoAcceptPairing }

    var pairingLockedOut: Bool {
        let now = MonoClock.nowMicros()
        failedPairings.removeAll { now - $0 > 600_000_000 }
        return failedPairings.count >= 5
    }

    func notePairingFailure() { failedPairings.append(MonoClock.nowMicros()) }

    func closePairingWindow() { pairingWindowUntil = 0 }

    // MARK: UI actions

    /// Lets a new device pair for `seconds` (plan §4.7: pairing only on explicit user action).
    public func openPairingWindow(seconds: TimeInterval = 120) {
        queue.async { [self] in pairingWindowUntil = MonoClock.nowMicros() + UInt64(seconds * 1_000_000) }
    }

    public func respondToPairing(accept: Bool) {
        queue.async { [self] in activeSession?.hostPairingDecision(accept) ?? sessions.values.forEach { $0.hostPairingDecision(accept) } }
    }

    public func respondToConsent(allow: Bool) {
        queue.async { [self] in activeSession?.consentDecision(allow) }
    }

    /// Hard kill switch (plan §4.7 item 3): disarm and release everything immediately.
    public func revokeInput() {
        queue.async { [self] in activeSession?.disarm(reason: "Input revoked on the host") }
    }

    public func endSession() {
        queue.async { [self] in activeSession?.close(reason: "Session ended on the host") }
    }

    public func removeDevice(_ id: String) {
        devices.remove(id)
        queue.async { [self] in
            if activeSession?.deviceID == id { activeSession?.close(reason: "Device removed on the host") }
        }
    }

    public func currentSnapshot() -> HostSession.Snapshot? { queue.sync { activeSession?.snapshot } }
}
#endif

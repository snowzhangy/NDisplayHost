import CryptoKit
import Foundation
import Network
import Security

/// QUIC configuration shared by host and client (core/proto/WIRE.md §2).
public enum WVDNet {
    public static let alpn = "wvd/1"
    public static let bonjourType = "_wvd._udp"
    public static let defaultPort: UInt16 = 47800
    public static let idleTimeoutMillis = 15_000

    public static func quicOptions() -> NWProtocolQUIC.Options {
        let o = NWProtocolQUIC.Options(alpn: [alpn])
        o.idleTimeout = idleTimeoutMillis
        o.maxDatagramFrameSize = 65_535
        o.initialMaxData = 16 << 20
        o.initialMaxStreamDataBidirectionalLocal = 4 << 20
        o.initialMaxStreamDataBidirectionalRemote = 4 << 20
        o.initialMaxStreamsBidirectional = 8
        return o
    }

    public static func parameters(_ quic: NWProtocolQUIC.Options) -> NWParameters {
        let p = NWParameters(quic: quic)
        // WMM AC_VI on Wi-Fi (plan §6.3).
        p.serviceClass = .interactiveVideo
        p.includePeerToPeer = false
        return p
    }

    /// Certificate hash of the TLS leaf from a sec_trust.
    public static func leafCertificateSHA256(_ trust: sec_trust_t) -> [UInt8]? {
        let t = sec_trust_copy_ref(trust).takeRetainedValue()
        guard let chain = SecTrustCopyCertificateChain(t) as? [SecCertificate], let leaf = chain.first else { return nil }
        return WVDCrypto.sha256(SecCertificateCopyData(leaf) as Data)
    }

    public static func describe(_ error: NWError) -> String {
        switch error {
        case .tls(let s): return "TLS error \(s)"
        case .posix(let c): return "network error \(c.rawValue) (\(c))"
        case .dns(let e): return "DNS error \(e)"
        default: return "\(error)"
        }
    }
}

/// Receives length-prefixed envelopes from a QUIC stream and sends framed envelopes on it.
public final class EnvelopeStream: @unchecked Sendable {
    public let connection: NWConnection
    private let queue: DispatchQueue
    private var deframer: StreamDeframer
    private var onEnvelope: (@Sendable (Envelope) -> Void)?
    private var onClose: (@Sendable (String?) -> Void)?
    private var sentPreamble = false
    private var closed = false

    public init(connection: NWConnection, queue: DispatchQueue, expectPreamble: Bool) {
        self.connection = connection
        self.queue = queue
        self.deframer = StreamDeframer(expectPreamble: expectPreamble)
    }

    /// Starts reading. `initial` = bytes already consumed while classifying the stream.
    public func start(initial: Data? = nil, onEnvelope: @escaping @Sendable (Envelope) -> Void, onClose: @escaping @Sendable (String?) -> Void) {
        self.onEnvelope = onEnvelope
        self.onClose = onClose
        if let initial, !initial.isEmpty { consume(initial) }
        receive()
    }

    private func receive() {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 1 << 20) { [weak self] data, _, isComplete, error in
            guard let self else { return }
            if let data, !data.isEmpty { self.consume(data) }
            if let error {
                self.finish(WVDNet.describe(error))
            } else if isComplete {
                self.finish(nil)
            } else if !self.closed {
                self.receive()
            }
        }
    }

    private func consume(_ data: Data) {
        do {
            for env in try deframer.append(data) { onEnvelope?(env) }
        } catch {
            finish("protocol error: \(error)")
            connection.cancel()
        }
    }

    private func finish(_ reason: String?) {
        guard !closed else { return }
        closed = true
        onClose?(reason)
        onEnvelope = nil
        onClose = nil
    }

    public func send(_ envelope: Envelope) {
        var data = Data()
        if !sentPreamble {
            data.append(contentsOf: StreamFraming.preamble)
            sentPreamble = true
        }
        data.append(StreamFraming.frame(envelope))
        connection.send(content: data, completion: .contentProcessed { _ in })
    }

    /// Marks the preamble as already sent (it was written by the caller).
    public func markPreambleSent() { sentPreamble = true }

    public func cancel() { connection.cancel() }
}

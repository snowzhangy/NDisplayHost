import CryptoKit
import Foundation
import Security

/// Pairing and per-connection authentication (plan §4.7; core/proto/WIRE.md §7).
///
/// Transport security is QUIC/TLS 1.3 with the host's self-signed certificate. On first contact the client
/// cannot know that certificate, so pairing binds the user's eyes to the TLS session: both sides derive a
/// 6-digit short authentication string (SAS) from a TLS exporter secret (RFC 5705/8446 §7.5) plus the
/// certificate hash and the device key. A man in the middle runs two different TLS sessions, so the two
/// screens show different codes. After pairing the client pins the certificate hash and the host stores the
/// device public key; every later connection is authenticated by an ECDSA signature over an exporter-bound
/// transcript, so a signature can never be replayed on another connection.
///
/// CryptoKit offers no PAKE, so this SAS scheme replaces the plan's "PAKE + PIN"; the host rate-limits and
/// only accepts pairing while the user has explicitly opened a pairing window.
public enum WVDCrypto {
    public static let exporterLength = 32
    public static let sasLabel = "EXPORTER-wvd-sas-v1"
    public static let authLabel = "EXPORTER-wvd-auth-v1"
    public static let pairLabel = "EXPORTER-wvd-pair-v1"

    /// TLS exporter secret for a QUIC/TLS connection.
    public static func exporter(_ metadata: sec_protocol_metadata_t, label: String) -> [UInt8]? {
        let bytes = Array(label.utf8)
        guard let data = bytes.withUnsafeBufferPointer({ buf -> dispatch_data_t? in
            buf.withMemoryRebound(to: CChar.self) { cbuf in
                sec_protocol_metadata_create_secret(metadata, bytes.count, cbuf.baseAddress!, exporterLength)
            }
        }) else { return nil }
        var out: [UInt8] = []
        out.reserveCapacity(exporterLength)
        (data as DispatchData).enumerateBytes { block, _, _ in out.append(contentsOf: block) }
        return out.count == exporterLength ? out : nil
    }

    public static func sha256(_ bytes: [UInt8]) -> [UInt8] { Array(SHA256.hash(data: bytes)) }
    public static func sha256(_ data: Data) -> [UInt8] { Array(SHA256.hash(data: data)) }

    /// Six decimal digits, identical on both ends of the *same* TLS session.
    public static func shortAuthenticationString(sasExporter: [UInt8], hostCertSHA256: [UInt8], devicePublicKey: [UInt8], digits: Int = 6) -> String {
        var info = Array("wvd-sas-v1".utf8)
        info += hostCertSHA256
        info += devicePublicKey
        let key = HKDF<SHA256>.deriveKey(inputKeyMaterial: SymmetricKey(data: sasExporter), salt: Array("wvd".utf8),
                                         info: info, outputByteCount: 8)
        let v = key.withUnsafeBytes { $0.loadUnaligned(as: UInt64.self) }
        var mod: UInt64 = 1
        for _ in 0..<digits { mod *= 10 }
        let n = UInt64(bigEndian: v) % mod
        let s = String(n)
        return String(repeating: "0", count: max(0, digits - s.count)) + s
    }

    public static func transcript(purpose: String, exporter: [UInt8], hostCertSHA256: [UInt8], devicePublicKey: [UInt8], deviceID: String) -> Data {
        var d = Data(purpose.utf8)
        d.append(0)
        d.append(contentsOf: exporter)
        d.append(contentsOf: hostCertSHA256)
        d.append(contentsOf: devicePublicKey)
        d.append(contentsOf: Array(deviceID.utf8))
        return d
    }

    public static func authTranscript(exporter: [UInt8], hostCertSHA256: [UInt8], devicePublicKey: [UInt8], deviceID: String) -> Data {
        transcript(purpose: "wvd-auth-v1", exporter: exporter, hostCertSHA256: hostCertSHA256, devicePublicKey: devicePublicKey, deviceID: deviceID)
    }

    public static func pairTranscript(exporter: [UInt8], hostCertSHA256: [UInt8], devicePublicKey: [UInt8], deviceID: String) -> Data {
        transcript(purpose: "wvd-pair-v1", exporter: exporter, hostCertSHA256: hostCertSHA256, devicePublicKey: devicePublicKey, deviceID: deviceID)
    }

    public static func verify(signature: [UInt8], transcript: Data, publicKeyX963: [UInt8]) -> Bool {
        guard let key = try? P256.Signing.PublicKey(x963Representation: publicKeyX963),
              let sig = try? P256.Signing.ECDSASignature(derRepresentation: signature) else { return false }
        return key.isValidSignature(sig, for: transcript)
    }

    /// Constant-time comparison for fingerprints.
    public static func equal(_ a: [UInt8], _ b: [UInt8]) -> Bool {
        guard a.count == b.count else { return false }
        var diff: UInt8 = 0
        for i in 0..<a.count { diff |= a[i] ^ b[i] }
        return diff == 0
    }

    public static func hex(_ bytes: [UInt8]) -> String { bytes.map { String(format: "%02x", $0) }.joined() }
}

#if os(macOS)
import CryptoKit
import Foundation
import Security
import WVDAppleCore

/// The host's long-term TLS identity: a self-signed P-256 certificate. The private key lives in the login
/// Keychain (found by application tag); the certificate DER is stored next to the paired-device list and the
/// identity is rebuilt from both at launch. Clients pin SHA-256(certificate DER) after pairing
/// (core/proto/WIRE.md §7), so this must survive restarts.
///
/// (Certificates added with `SecItemAdd` keep their common name as `kSecAttrLabel`, so a label lookup
/// cannot find them; persisting the DER avoids depending on Keychain certificate queries at all.)
public final class HostIdentity: @unchecked Sendable {
    public enum IdentityError: Error {
        case key(String)
        case certificate
        case keychain(OSStatus)
    }

    public let identity: SecIdentity
    public let certificateDER: Data
    public let certSHA256: [UInt8]
    public let hostID: String
    let keyTag: String
    let certURL: URL?
    private let certRef: SecCertificate

    init(identity: SecIdentity, certificate: SecCertificate, certificateDER: Data, keyTag: String, certURL: URL?) {
        self.identity = identity
        self.certRef = certificate
        self.certificateDER = certificateDER
        self.certSHA256 = WVDCrypto.sha256(certificateDER)
        self.hostID = HostIdentity.hostID(for: certificateDER)
        self.keyTag = keyTag
        self.certURL = certURL
    }

    public var fingerprint: String { WVDCrypto.hex(Array(certSHA256.prefix(8))) }

    public static var defaultDirectory: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!.appendingPathComponent("WVDHost")
    }

    /// Loads the persistent identity or creates one on first run (or if its key disappeared).
    public static func loadOrCreate(directory: URL = HostIdentity.defaultDirectory, tag: String = "com.cinetec.wvm.host.identity",
                                    commonName: String = "WVD Host") throws -> HostIdentity {
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let url = directory.appendingPathComponent("host-certificate.der")
        if let existing = try? load(certURL: url, tag: tag) { return existing }
        return try create(tag: tag, commonName: commonName, certURL: url)
    }

    /// Throwaway identity for tests; call `delete()` when done.
    public static func ephemeral() throws -> HostIdentity {
        try create(tag: "com.cinetec.wvm.host.test.\(UUID().uuidString)", commonName: "WVD Test Host", certURL: nil)
    }

    static func load(certURL: URL, tag: String) throws -> HostIdentity {
        let der = try Data(contentsOf: certURL)
        guard let cert = SecCertificateCreateWithData(nil, der as CFData) else { throw IdentityError.certificate }
        // The private key must still be there (and match): SecIdentityCreateWithCertificate looks it up by
        // the certificate's public key.
        guard keyExists(tag: tag) else { throw IdentityError.keychain(errSecItemNotFound) }
        var identity: SecIdentity?
        let status = SecIdentityCreateWithCertificate(nil, cert, &identity)
        guard status == errSecSuccess, let identity else { throw IdentityError.keychain(status) }
        return HostIdentity(identity: identity, certificate: cert, certificateDER: der, keyTag: tag, certURL: certURL)
    }

    static func keyExists(tag: String) -> Bool {
        let q: [CFString: Any] = [kSecClass: kSecClassKey, kSecAttrApplicationTag: Data(tag.utf8), kSecMatchLimit: kSecMatchLimitOne]
        return SecItemCopyMatching(q as CFDictionary, nil) == errSecSuccess
    }

    static func hostID(for der: Data) -> String {
        WVDCrypto.hex(Array(WVDCrypto.sha256(der).prefix(12)))
    }

    static func create(tag: String, commonName: String, certURL: URL?) throws -> HostIdentity {
        // A stale key under this tag (e.g. after the certificate file was deleted) would be ambiguous.
        SecItemDelete([kSecClass: kSecClassKey, kSecAttrApplicationTag: Data(tag.utf8)] as CFDictionary)
        var error: Unmanaged<CFError>?
        let attrs: [CFString: Any] = [
            kSecAttrKeyType: kSecAttrKeyTypeECSECPrimeRandom,
            kSecAttrKeySizeInBits: 256,
            kSecPrivateKeyAttrs: [
                kSecAttrIsPermanent: true,
                kSecAttrLabel: tag,
                kSecAttrApplicationTag: Data(tag.utf8),
            ] as [CFString: Any],
        ]
        guard let priv = SecKeyCreateRandomKey(attrs as CFDictionary, &error) else {
            throw IdentityError.key(String(describing: error?.takeRetainedValue()))
        }
        guard let pub = SecKeyCopyPublicKey(priv), let pubRaw = SecKeyCopyExternalRepresentation(pub, &error) as Data? else {
            throw IdentityError.key("public key")
        }
        let spki = try P256.Signing.PublicKey(x963Representation: pubRaw).derRepresentation

        // Minimal X.509 v3: version, random serial, ecdsa-with-SHA256, CN, 20-year validity, SPKI.
        var serial: [UInt8] = (0..<12).map { _ in UInt8.random(in: 0...255) }
        serial[0] &= 0x7F
        let name = DER.sequence(DER.set(DER.sequence(DER.oid([0x55, 0x04, 0x03]), DER.utf8(commonName))))
        let alg = DER.sequence(DER.oid([0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02]))
        let tbs = DER.sequence(
            DER.explicit(0, DER.integer([0x02])),
            DER.integer(serial),
            alg,
            name,
            DER.sequence(DER.generalizedTime(Date(timeIntervalSinceNow: -86_400)), DER.generalizedTime(Date(timeIntervalSinceNow: 86_400 * 365 * 20))),
            name,
            Array(spki))
        guard let sig = SecKeyCreateSignature(priv, .ecdsaSignatureMessageX962SHA256, Data(tbs) as CFData, &error) as Data? else {
            throw IdentityError.key("sign")
        }
        let der = Data(DER.sequence(tbs, alg, DER.bitString(Array(sig))))
        guard let cert = SecCertificateCreateWithData(nil, der as CFData) else { throw IdentityError.certificate }
        var identity: SecIdentity?
        let status = SecIdentityCreateWithCertificate(nil, cert, &identity)
        guard status == errSecSuccess, let identity else { throw IdentityError.keychain(status) }
        if let certURL { try der.write(to: certURL, options: .atomic) }
        return HostIdentity(identity: identity, certificate: cert, certificateDER: der, keyTag: tag, certURL: certURL)
    }

    /// Removes the private key (by tag) and the stored certificate. Nothing else is left in the Keychain.
    public func delete() {
        SecItemDelete([kSecClass: kSecClassKey, kSecAttrApplicationTag: Data(keyTag.utf8)] as CFDictionary)
        SecItemDelete([kSecClass: kSecClassCertificate, kSecValueRef: certRef] as CFDictionary)
        if let certURL { try? FileManager.default.removeItem(at: certURL) }
    }
}

/// Just enough DER for a self-signed certificate.
enum DER {
    static func length(_ n: Int) -> [UInt8] {
        if n < 0x80 { return [UInt8(n)] }
        var b: [UInt8] = []
        var v = n
        while v > 0 { b.insert(UInt8(v & 0xFF), at: 0); v >>= 8 }
        return [0x80 | UInt8(b.count)] + b
    }
    static func tlv(_ tag: UInt8, _ content: [UInt8]) -> [UInt8] { [tag] + length(content.count) + content }
    static func sequence(_ items: [UInt8]...) -> [UInt8] { tlv(0x30, items.flatMap { $0 }) }
    static func set(_ items: [UInt8]...) -> [UInt8] { tlv(0x31, items.flatMap { $0 }) }
    static func oid(_ bytes: [UInt8]) -> [UInt8] { tlv(0x06, bytes) }
    static func utf8(_ s: String) -> [UInt8] { tlv(0x0C, Array(s.utf8)) }
    static func integer(_ bytes: [UInt8]) -> [UInt8] { tlv(0x02, bytes) }
    static func bitString(_ bytes: [UInt8]) -> [UInt8] { tlv(0x03, [0] + bytes) }
    static func explicit(_ n: UInt8, _ content: [UInt8]) -> [UInt8] { tlv(0xA0 | n, content) }
    static func generalizedTime(_ d: Date) -> [UInt8] {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.timeZone = TimeZone(identifier: "UTC")
        f.dateFormat = "yyyyMMddHHmmss'Z'"
        return tlv(0x18, Array(f.string(from: d).utf8))
    }
}

/// Devices allowed to connect: device ID -> public key (plan §4.7).
public struct PairedDevice: Codable, Sendable, Hashable, Identifiable {
    public var deviceID: String
    public var name: String
    public var model: String
    public var publicKeyX963: [UInt8]
    public var pairedAt: Date
    public var lastSeen: Date?
    public var id: String { deviceID }
}

public final class PairedDeviceStore: @unchecked Sendable {
    private let url: URL?
    private let state: Locked<[String: PairedDevice]>

    public init(url: URL?) {
        self.url = url
        var devices: [String: PairedDevice] = [:]
        if let url, let data = try? Data(contentsOf: url), let list = try? JSONDecoder().decode([PairedDevice].self, from: data) {
            for d in list { devices[d.deviceID] = d }
        }
        state = Locked(devices)
    }

    public static func standard() -> PairedDeviceStore {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!.appendingPathComponent("WVDHost")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return PairedDeviceStore(url: dir.appendingPathComponent("paired-devices.json"))
    }

    public static func inMemory() -> PairedDeviceStore { PairedDeviceStore(url: nil) }

    public var all: [PairedDevice] { state.snapshot.values.sorted { $0.pairedAt < $1.pairedAt } }

    public func device(_ id: String) -> PairedDevice? { state.withLock { $0[id] } }

    public func save(_ d: PairedDevice) {
        state.withLock { $0[d.deviceID] = d }
        persist()
    }

    public func remove(_ id: String) {
        state.withLock { $0[id] = nil }
        persist()
    }

    public func touch(_ id: String) {
        state.withLock { $0[id]?.lastSeen = Date() }
        persist()
    }

    private func persist() {
        guard let url else { return }
        if let data = try? JSONEncoder().encode(all) { try? data.write(to: url, options: .atomic) }
    }
}
#endif

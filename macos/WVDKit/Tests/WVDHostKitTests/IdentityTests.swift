import Foundation
import Security
import Testing
@testable import WVDHostKit

@Suite struct IdentityTests {
    /// Clients pin the certificate hash, so the identity must survive a host restart.
    @Test func identityPersistsAcrossLoadsAndDeletesCleanly() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("wvd-id-\(UUID().uuidString)")
        let tag = "com.cinetec.wvm.host.test.persist.\(UUID().uuidString)"
        let first = try HostIdentity.loadOrCreate(directory: dir, tag: tag, commonName: "WVD Test Host")
        let second = try HostIdentity.loadOrCreate(directory: dir, tag: tag, commonName: "WVD Test Host")
        #expect(first.certSHA256 == second.certSHA256)
        #expect(first.hostID == second.hostID)
        second.delete()
        #expect(!HostIdentity.keyExists(tag: tag))
        #expect(!FileManager.default.fileExists(atPath: dir.appendingPathComponent("host-certificate.der").path))
        // No certificate with the test common name is left behind.
        let q: [CFString: Any] = [kSecClass: kSecClassCertificate, kSecAttrLabel: "WVD Test Host", kSecMatchLimit: kSecMatchLimitAll]
        #expect(SecItemCopyMatching(q as CFDictionary, nil) == errSecItemNotFound)
        try? FileManager.default.removeItem(at: dir)
    }
}

@Suite struct ServerLifecycleTests {
    /// The host app restarts its listener when settings change; the fixed port must come back.
    @Test func restartOnSamePort() throws {
        let identity = try HostIdentity.ephemeral()
        defer { identity.delete() }
        let port = UInt16.random(in: 50_000...60_000)
        var config = HostServer.Config()
        config.port = port
        config.advertise = false
        for _ in 0..<3 {
            let server = try HostServer(identity: identity, devices: .inMemory(), injector: RecordingInjector(), config: config) {
                try SyntheticSource(width: 320, height: 180)
            }
            try server.start()
            #expect(server.waitUntilListening() == port)
            let stopped = DispatchSemaphore(value: 0)
            server.stop { stopped.signal() }
            #expect(stopped.wait(timeout: .now() + 5) == .success)
        }
    }
}

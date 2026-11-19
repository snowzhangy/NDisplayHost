import AppKit
import Carbon.HIToolbox
import Observation
import SwiftUI
import WVDAppleCore
import WVDHostKit

@main
struct WVDHostApp: App {
    @State private var model = HostModel()

    var body: some Scene {
        MenuBarExtra {
            HostPanel().environment(model)
        } label: {
            Image(systemName: model.menuSymbol)
        }
        .menuBarExtraStyle(.window)
    }
}

enum SourceChoice: Hashable, Identifiable {
    case synthetic
    case display(CGDirectDisplayID, String)
    var id: String {
        switch self {
        case .synthetic: return "synthetic"
        case .display(let id, _): return "display-\(id)"
        }
    }
    var label: String {
        switch self {
        case .synthetic: return "Test pattern (synthetic)"
        case .display(_, let name): return name
        }
    }
}

@MainActor
@Observable
final class HostModel {
    var status: HostServer.Status = .stopped
    var session: HostSession.Snapshot?
    var pairing: HostServer.PairingRequest?
    var consentDevice: String?
    var log: [String] = []
    var devices: [PairedDevice] = []
    var sources: [SourceChoice] = [.synthetic]
    var source: SourceChoice = .synthetic { didSet { restart() } }
    var fps = 60 { didSet { restart() } }
    var maxMbps = 150.0 { didSet { restart() } }
    var accessibilityTrusted = false
    var error: String?
    var killSwitchError: String?

    private var server: HostServer?
    private let store = PairedDeviceStore.standard()
    private var identity: HostIdentity?
    private let injector = CGEventInjector()
    private var hotKeyRef: EventHotKeyRef?
    private var hotKeyHandler: EventHandlerRef?

    init() {
        HostModel.current = self
        devices = store.all
        accessibilityTrusted = injector.isTrusted
        do {
            identity = try HostIdentity.loadOrCreate()
        } catch {
            self.error = "Could not create the host identity in the Keychain: \(error)"
        }
        Task { await refreshDisplays() }
        installKillSwitch()
        restart()
    }

    var fingerprint: String { identity?.fingerprint ?? "-" }

    var hasPendingRequest: Bool { pairing != nil || consentDevice != nil }

    var menuSymbol: String {
        if hasPendingRequest { return "exclamationmark.circle.fill" }
        if session?.input == .armed { return "display.and.arrow.down" }
        if session != nil { return "display" }
        return "rectangle.on.rectangle"
    }

    func refreshDisplays() async {
        guard let displays = try? await ScreenCaptureSource.displays() else { return }
        sources = [.synthetic] + displays.map { .display($0.displayID, "\($0.name) (\($0.pixelWidth)\u{00D7}\($0.pixelHeight))") }
    }

    private var restartWork: DispatchWorkItem?

    /// Settings changes restart the listener; debounced so a slider drag restarts once, and the new
    /// listener binds only after the old one released the port.
    func restart() {
        restartWork?.cancel()
        let work = DispatchWorkItem { [weak self] in
            MainActor.assumeIsolated {
                guard let self else { return }
                if let old = self.server {
                    self.server = nil
                    old.stop { Self.main { $0.startServer() } }
                } else {
                    self.startServer()
                }
            }
        }
        restartWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + (server == nil ? 0 : 0.4), execute: work)
    }

    private func startServer() {
        guard server == nil, let identity else { return }
        var config = HostServer.Config()
        config.fps = fps
        config.maxBitrateBps = Int(maxMbps * 1_000_000)
        config.startBitrateBps = min(config.startBitrateBps, config.maxBitrateBps)
        var events = HostServer.Events()
        events.status = { s in Self.main { $0.status = s } }
        events.session = { s in Self.main { $0.session = s; $0.devices = $0.store.all } }
        events.pairingRequest = { r in Self.main { m in
            m.pairing = r
            if r != nil { m.showPrompt() } else { m.hidePromptIfIdle() }
        } }
        events.consentRequest = { d in Self.main { m in
            m.consentDevice = d
            if d != nil { m.showPrompt() } else { m.hidePromptIfIdle() }
        } }
        events.log = { line in Self.main { m in
            m.log.append(line)
            if m.log.count > 50 { m.log.removeFirst() }
        } }
        let choice = source
        let fps = self.fps
        do {
            let server = try HostServer(identity: identity, devices: store, injector: injector, config: config, events: events) {
                switch choice {
                case .synthetic:
                    return try SyntheticSource(width: 2560, height: 1440, refreshHz: fps)
                case .display(let id, _):
                    let displays = try await ScreenCaptureSource.displays()
                    guard let d = displays.first(where: { $0.displayID == id }) else { throw CocoaError(.featureUnsupported) }
                    return ScreenCaptureSource(display: d, fps: fps)
                }
            }
            try server.start()
            self.server = server
            error = nil
        } catch {
            self.error = "Could not start: \(error.localizedDescription)"
        }
    }

    nonisolated static func main(_ body: @escaping @MainActor (HostModel) -> Void) {
        DispatchQueue.main.async {
            MainActor.assumeIsolated {
                if let m = HostModel.current { body(m) }
            }
        }
    }

    static weak var current: HostModel?

    func openPairing() { server?.openPairingWindow(seconds: 120) }
    func pairingDecision(_ accept: Bool) {
        server?.respondToPairing(accept: accept)
        pairing = nil
        hidePromptIfIdle()
    }
    func consentDecision(_ allow: Bool) {
        if allow && !injector.isTrusted { CGEventInjector.promptForAccessibility() }
        server?.respondToConsent(allow: allow)
        consentDevice = nil
        accessibilityTrusted = injector.isTrusted
        hidePromptIfIdle()
    }
    func revoke() { server?.revokeInput() }
    func endSession() { server?.endSession() }
    func remove(_ d: PairedDevice) { server?.removeDevice(d.deviceID); devices = store.all }

    private var promptWindow: NSWindow?

    /// Pairing and consent need the user even when the menu is closed: show a floating window.
    func showPrompt() {
        if promptWindow == nil {
            let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 440, height: 320), styleMask: [.titled, .closable],
                             backing: .buffered, defer: false)
            w.title = "WVD Host"
            w.isReleasedWhenClosed = false
            w.level = .floating
            w.contentView = NSHostingView(rootView: PromptView().environment(self))
            w.center()
            promptWindow = w
        }
        NSApp.activate()
        promptWindow?.makeKeyAndOrderFront(nil)
    }

    func hidePromptIfIdle() {
        if !hasPendingRequest { promptWindow?.orderOut(nil) }
    }

    /// Kill switch ⌃⌥⌘K revokes input immediately (plan §4.7 item 3).
    ///
    /// A system hot key, not an NSEvent monitor: a global monitor never sees events sent to this app (so it
    /// fails exactly when the host's own prompt or panel has focus) and needs Accessibility. A registered hot
    /// key fires whichever app is frontmost, including this one, and is consumed so it never reaches the
    /// focused app. It is keyed by virtual key code, so it works on every keyboard layout position of K.
    private func installKillSwitch() {
        var spec = EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyPressed))
        let handler: EventHandlerUPP = { _, _, _ in
            DispatchQueue.main.async { MainActor.assumeIsolated { HostModel.current?.revoke() } }
            return noErr
        }
        var handlerRef: EventHandlerRef?
        InstallEventHandler(GetApplicationEventTarget(), handler, 1, &spec, nil, &handlerRef)
        var ref: EventHotKeyRef?
        let id = EventHotKeyID(signature: OSType(0x5756_444B), id: 1)  // 'WVDK'
        let status = RegisterEventHotKey(UInt32(kVK_ANSI_K), UInt32(controlKey | optionKey | cmdKey), id,
                                         GetApplicationEventTarget(), 0, &ref)
        if status != noErr {
            killSwitchError = "Could not register the \u{2303}\u{2325}\u{2318}K kill switch (\(status)). Use Revoke Input in this menu."
        }
        hotKeyRef = ref
        hotKeyHandler = handlerRef
    }
}

struct HostPanel: View {
    @Environment(HostModel.self) private var model

    var body: some View {
        @Bindable var model = model
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("WVD Host").font(.headline)
                Spacer()
                statusPill
            }
            if let error = model.error { Text(error).foregroundStyle(.red).font(.caption) }
            if let e = model.killSwitchError { Text(e).foregroundStyle(.red).font(.caption) }
            if model.hasPendingRequest {
                GroupBox { RequestControls() }
            }

            if let s = model.session {
                GroupBox {
                    VStack(alignment: .leading, spacing: 6) {
                        HStack {
                            Image(systemName: "visionpro")
                            Text(s.deviceName).bold()
                            Spacer()
                            armedBadge(s.input)
                        }
                        Text("\(s.width)\u{00D7}\(s.height) \u{00B7} \(Int(s.fps)) fps \u{00B7} \(String(format: "%.0f", s.bitrateMbps)) / \(String(format: "%.0f", s.targetMbps)) Mbit/s")
                            .font(.caption.monospacedDigit())
                        Text("encode p95 \(String(format: "%.1f", s.encodeP95Millis)) ms \u{00B7} RTT \(String(format: "%.1f", s.rttMillis)) ms \u{00B7} e2e p95 \(String(format: "%.0f", s.e2eP95Millis)) ms \u{00B7} loss \(String(format: "%.2f", s.lossPercent))%")
                            .font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                        HStack {
                            if s.input == .armed {
                                Button("Revoke Input (\u{2303}\u{2325}\u{2318}K)", role: .destructive) { model.revoke() }
                            }
                            Button("End Session") { model.endSession() }
                        }
                    }
                }
            }

            Picker("Source", selection: $model.source) {
                ForEach(model.sources) { Text($0.label).tag($0) }
            }
            Picker("Frame rate", selection: $model.fps) {
                Text("60 fps").tag(60)
                Text("90 fps").tag(90)
                Text("120 fps").tag(120)
            }
            HStack {
                Text("Max bitrate")
                Slider(value: $model.maxMbps, in: 20...300, step: 10)
                Text("\(Int(model.maxMbps))").monospacedDigit().frame(width: 36)
            }

            Divider()
            HStack {
                Button("Pair New Device\u{2026}") {
                    model.openPairing()
                    model.showPrompt()
                }
                Spacer()
                Text("ID \(model.fingerprint)").font(.caption.monospaced()).foregroundStyle(.secondary)
            }
            if !model.devices.isEmpty {
                Text("Paired devices").font(.subheadline)
                ForEach(model.devices) { d in
                    HStack {
                        Text(d.name)
                        Spacer()
                        Button("Remove", role: .destructive) { model.remove(d) }.buttonStyle(.link)
                    }
                }
            }
            if !model.accessibilityTrusted {
                Text("Input from the headset needs Accessibility permission (System Settings \u{203A} Privacy & Security \u{203A} Accessibility).")
                    .font(.caption).foregroundStyle(.orange)
            }
            Divider()
            Button("Quit") { NSApp.terminate(nil) }
        }
        .padding(14)
        .frame(width: 380)
    }

    @ViewBuilder private var statusPill: some View {
        switch model.status {
        case .listening(let port): Text("Listening :\(port)").font(.caption).foregroundStyle(.green)
        case .failed(let e): Text(e).font(.caption).foregroundStyle(.red)
        case .stopped: Text("Stopped").font(.caption).foregroundStyle(.secondary)
        }
    }

    @ViewBuilder private func armedBadge(_ s: InputArmState) -> some View {
        switch s {
        case .armed: Label("Input armed", systemImage: "exclamationmark.shield.fill").foregroundStyle(.orange).font(.caption)
        case .consentPending: Text("Awaiting consent").font(.caption)
        default: Text("View only").font(.caption).foregroundStyle(.secondary)
        }
    }
}

/// Pairing (SAS compare) and per-connection input consent (plan §4.7 items 1-2). Shown both in the menu
/// panel and in the floating prompt window.
struct RequestControls: View {
    @Environment(HostModel.self) private var model

    var body: some View {
        VStack(spacing: 14) {
            if let p = model.pairing {
                Image(systemName: "lock.shield").font(.system(size: 32))
                Text("Pair \(p.deviceName)?").font(.title3.bold())
                Text("Make sure the headset shows the same code:").foregroundStyle(.secondary)
                Text("\(p.sas.prefix(3)) \(p.sas.suffix(3))").font(.system(size: 46, weight: .semibold, design: .monospaced))
                HStack {
                    Button("Reject", role: .cancel) { model.pairingDecision(false) }
                    Button("Codes Match") { model.pairingDecision(true) }.keyboardShortcut(.defaultAction)
                }
            } else if let d = model.consentDevice {
                Image(systemName: "hand.raised").font(.system(size: 32))
                Text("Allow \(d) to control this Mac?").font(.title3.bold())
                Text("Video streams either way. Input can be revoked any time with \u{2303}\u{2325}\u{2318}K.")
                    .foregroundStyle(.secondary).multilineTextAlignment(.center)
                HStack {
                    Button("View Only", role: .cancel) { model.consentDecision(false) }
                    Button("Allow Control") { model.consentDecision(true) }.keyboardShortcut(.defaultAction)
                }
            }
        }
        .frame(maxWidth: .infinity)
    }
}

struct PromptView: View {
    @Environment(HostModel.self) private var model

    var body: some View {
        VStack(spacing: 18) {
            if model.hasPendingRequest {
                RequestControls()
            } else {
                Image(systemName: "visionpro").font(.system(size: 40))
                Text("Waiting for a headset\u{2026}").font(.title3)
                Text("Open Virtual Monitor on Apple Vision Pro and choose this computer. Pairing stays open for two minutes.")
                    .foregroundStyle(.secondary).multilineTextAlignment(.center)
            }
        }
        .padding(28)
        .frame(width: 440)
    }
}

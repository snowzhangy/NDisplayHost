#if os(macOS)
import ApplicationServices
import CoreGraphics
import Foundation
import WVDAppleCore

/// Applies validated, typed input events (plan §4.7 item 5: only typed events reach the injector).
public protocol InputInjector: AnyObject, Sendable {
    /// `monitorPixels` = the monitor's pixel size; `desktopFrame` = its rect in global display points.
    func apply(_ event: InputEvent.Kind, monitorPixels: CGSize, desktopFrame: CGRect)
    /// Releases every held key and button (plan §4.4 input-state release).
    func releaseAll()
    var isTrusted: Bool { get }
    var heldDescription: String { get }
}

/// Records events instead of injecting them (tests, and "video only" mode).
public final class RecordingInjector: InputInjector, @unchecked Sendable {
    public let events = Locked<[InputEvent.Kind]>([])
    public let releases = Locked(0)
    public init() {}
    public func apply(_ event: InputEvent.Kind, monitorPixels: CGSize, desktopFrame: CGRect) { events.withLock { $0.append(event) } }
    public func releaseAll() { releases.withLock { $0 += 1 } }
    public var isTrusted: Bool { true }
    public var heldDescription: String { "" }
}

/// CGEvent-based injection. Needs Accessibility permission (System Settings > Privacy & Security).
public final class CGEventInjector: InputInjector, @unchecked Sendable {
    private struct Held {
        var keys: Set<UInt32> = []
        var buttons: Set<UInt32> = []
        var lastPoint = CGPoint.zero
        var lastClick: (button: UInt32, at: UInt64, point: CGPoint, count: Int64)?
    }

    private let state = Locked(Held())
    private let source = CGEventSource(stateID: .hidSystemState)

    public init() {}

    public var isTrusted: Bool { AXIsProcessTrusted() }

    public static func promptForAccessibility() {
        let key = "AXTrustedCheckOptionPrompt"  // kAXTrustedCheckOptionPrompt (a C global, not concurrency-safe)
        _ = AXIsProcessTrustedWithOptions([key: true] as CFDictionary)
    }

    public var heldDescription: String {
        let s = state.snapshot
        return "keys=\(s.keys.count) buttons=\(s.buttons.count)"
    }

    private func point(_ x: Float, _ y: Float, _ px: CGSize, _ frame: CGRect) -> CGPoint? {
        guard x.isFinite, y.isFinite, px.width > 0, px.height > 0 else { return nil }
        let cx = min(max(Double(x), 0), Double(px.width) - 1)
        let cy = min(max(Double(y), 0), Double(px.height) - 1)
        return CGPoint(x: frame.minX + cx * frame.width / px.width, y: frame.minY + cy * frame.height / px.height)
    }

    public func apply(_ event: InputEvent.Kind, monitorPixels: CGSize, desktopFrame: CGRect) {
        switch event {
        case .move(let m):
            guard let p = point(m.x, m.y, monitorPixels, desktopFrame) else { return }
            let held = state.withLock { s -> Set<UInt32> in s.lastPoint = p; return s.buttons }
            let type: CGEventType = held.contains(MouseButton.left.rawValue) ? .leftMouseDragged
                : held.contains(MouseButton.right.rawValue) ? .rightMouseDragged
                : held.contains(MouseButton.middle.rawValue) ? .otherMouseDragged : .mouseMoved
            post(CGEvent(mouseEventSource: source, mouseType: type, mouseCursorPosition: p, mouseButton: .left))
        case .button(let b):
            guard let p = point(b.x, b.y, monitorPixels, desktopFrame) else { return }
            let (type, cgButton) = Self.mouseType(b.button, down: b.down)
            let clickCount: Int64 = state.withLock { s in
                s.lastPoint = p
                if b.down { s.buttons.insert(b.button.rawValue) } else { s.buttons.remove(b.button.rawValue) }
                guard b.down else { return s.lastClick?.count ?? 1 }
                let now = MonoClock.nowMicros()
                if let last = s.lastClick, last.button == b.button.rawValue, now - last.at < 500_000,
                   hypot(last.point.x - p.x, last.point.y - p.y) < 6 {
                    s.lastClick = (b.button.rawValue, now, p, min(last.count + 1, 3))
                } else {
                    s.lastClick = (b.button.rawValue, now, p, 1)
                }
                return s.lastClick!.count
            }
            let e = CGEvent(mouseEventSource: source, mouseType: type, mouseCursorPosition: p, mouseButton: cgButton)
            e?.setIntegerValueField(.mouseEventClickState, value: Int64(b.clickCount > 0 ? Int64(b.clickCount) : clickCount))
            post(e)
        case .scroll(let s):
            let dy = Self.wheelDelta(s.dy), dx = Self.wheelDelta(s.dx)
            guard dx != 0 || dy != 0 else { return }
            let e = CGEvent(scrollWheelEvent2Source: source, units: s.precise ? .pixel : .line, wheelCount: 2,
                            wheel1: dy, wheel2: dx, wheel3: 0)
            if let e, s.precise { e.setIntegerValueField(.scrollWheelEventIsContinuous, value: 1) }
            post(e)
        case .key(let k):
            guard let code = HIDKeyMap.macKeyCode(forUsage: k.hidUsage) else { return }
            let flags = state.withLock { s -> CGEventFlags in
                if k.down { s.keys.insert(k.hidUsage) } else { s.keys.remove(k.hidUsage) }
                return HIDKeyMap.flags(for: s.keys)
            }
            let e = CGEvent(keyboardEventSource: source, virtualKey: code, keyDown: k.down)
            e?.flags = flags
            post(e)
        case .text(let t):
            for chunk in Self.chunks(t.text) {
                var utf16 = Array(chunk.utf16)
                for down in [true, false] {
                    let e = CGEvent(keyboardEventSource: source, virtualKey: 0, keyDown: down)
                    e?.keyboardSetUnicodeString(stringLength: utf16.count, unicodeString: &utf16)
                    post(e)
                }
            }
        }
    }

    /// Largest scroll step accepted per event (pixels or lines).
    static let maxWheelDelta: Float = 10_000

    /// Network floats -> wheel delta. NaN/infinity become 0 and the range is clamped *before* converting,
    /// because `Int32(_:)` traps on non-finite or out-of-range values.
    static func wheelDelta(_ v: Float) -> Int32 {
        guard v.isFinite else { return 0 }
        return Int32(min(max(v, -maxWheelDelta), maxWheelDelta).rounded())
    }

    static func chunks(_ s: String) -> [String] {
        // CGEvent carries at most 20 UTF-16 units per event.
        var out: [String] = []
        var cur = ""
        for ch in s {
            if (cur + String(ch)).utf16.count > 20 { out.append(cur); cur = "" }
            cur.append(ch)
        }
        if !cur.isEmpty { out.append(cur) }
        return out
    }

    static func mouseType(_ b: MouseButton, down: Bool) -> (CGEventType, CGMouseButton) {
        switch b {
        case .left: return (down ? .leftMouseDown : .leftMouseUp, .left)
        case .right: return (down ? .rightMouseDown : .rightMouseUp, .right)
        case .middle: return (down ? .otherMouseDown : .otherMouseUp, .center)
        }
    }

    private func post(_ e: CGEvent?) { e?.post(tap: .cghidEventTap) }

    public func releaseAll() {
        let held = state.withLock { s -> Held in
            let copy = s
            s.keys.removeAll()
            s.buttons.removeAll()
            return copy
        }
        for usage in held.keys {
            if let code = HIDKeyMap.macKeyCode(forUsage: usage) {
                post(CGEvent(keyboardEventSource: source, virtualKey: code, keyDown: false))
            }
        }
        for raw in held.buttons {
            guard let b = MouseButton(rawValue: raw) else { continue }
            let (type, cg) = Self.mouseType(b, down: false)
            post(CGEvent(mouseEventSource: source, mouseType: type, mouseCursorPosition: held.lastPoint, mouseButton: cg))
        }
    }
}

/// USB HID keyboard usages (page 0x07) to macOS virtual key codes. The client sends HID usages, which
/// are what `GCKeyCode` values are on visionOS; the Windows host maps the same usages to scan codes.
public enum HIDKeyMap {
    static let table: [UInt32: CGKeyCode] = {
        var t: [UInt32: CGKeyCode] = [:]
        let letters: [CGKeyCode] = [0x00, 0x0B, 0x08, 0x02, 0x0E, 0x03, 0x05, 0x04, 0x22, 0x26, 0x28, 0x25, 0x2E,
                                    0x2D, 0x1F, 0x23, 0x0C, 0x0F, 0x01, 0x11, 0x20, 0x09, 0x0D, 0x07, 0x10, 0x06]
        for (i, c) in letters.enumerated() { t[0x04 + UInt32(i)] = c }
        let digits: [CGKeyCode] = [0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19, 0x1D]  // 1..9, 0
        for (i, c) in digits.enumerated() { t[0x1E + UInt32(i)] = c }
        let rest: [(UInt32, CGKeyCode)] = [
            (0x28, 0x24), (0x29, 0x35), (0x2A, 0x33), (0x2B, 0x30), (0x2C, 0x31), (0x2D, 0x1B), (0x2E, 0x18),
            (0x2F, 0x21), (0x30, 0x1E), (0x31, 0x2A), (0x32, 0x2A), (0x33, 0x29), (0x34, 0x27), (0x35, 0x32),
            (0x36, 0x2B), (0x37, 0x2F), (0x38, 0x2C), (0x39, 0x39),
            (0x3A, 0x7A), (0x3B, 0x78), (0x3C, 0x63), (0x3D, 0x76), (0x3E, 0x60), (0x3F, 0x61), (0x40, 0x62),
            (0x41, 0x64), (0x42, 0x65), (0x43, 0x6D), (0x44, 0x67), (0x45, 0x6F),
            (0x49, 0x72), (0x4A, 0x73), (0x4B, 0x74), (0x4C, 0x75), (0x4D, 0x77), (0x4E, 0x79),
            (0x4F, 0x7C), (0x50, 0x7B), (0x51, 0x7D), (0x52, 0x7E),
            (0x53, 0x47), (0x54, 0x4B), (0x55, 0x43), (0x56, 0x4E), (0x57, 0x45), (0x58, 0x4C),
            (0x59, 0x53), (0x5A, 0x54), (0x5B, 0x55), (0x5C, 0x56), (0x5D, 0x57), (0x5E, 0x58), (0x5F, 0x59),
            (0x60, 0x5B), (0x61, 0x5C), (0x62, 0x52), (0x63, 0x41), (0x64, 0x0A), (0x67, 0x51),
            (0x68, 0x69), (0x69, 0x6B), (0x6A, 0x71), (0x6B, 0x6A), (0x6C, 0x40), (0x6D, 0x4F), (0x6E, 0x50),
            (0xE0, 0x3B), (0xE1, 0x38), (0xE2, 0x3A), (0xE3, 0x37), (0xE4, 0x3E), (0xE5, 0x3C), (0xE6, 0x3D), (0xE7, 0x36),
        ]
        for (u, c) in rest { t[u] = c }
        return t
    }()

    public static func macKeyCode(forUsage usage: UInt32) -> CGKeyCode? { table[usage] }

    /// Modifier flags for the currently held usages. On a Windows keyboard layout the Windows/GUI key maps
    /// to Command and Alt to Option, which is what a Mac user expects from an external PC keyboard.
    static func flags(for held: Set<UInt32>) -> CGEventFlags {
        var f: CGEventFlags = []
        if held.contains(0xE0) || held.contains(0xE4) { f.insert(.maskControl) }
        if held.contains(0xE1) || held.contains(0xE5) { f.insert(.maskShift) }
        if held.contains(0xE2) || held.contains(0xE6) { f.insert(.maskAlternate) }
        if held.contains(0xE3) || held.contains(0xE7) { f.insert(.maskCommand) }
        return f
    }
}
#endif

import Foundation
import os

/// Small lock-protected box. Real-time media paths use locks rather than actors: an actor hop costs a
/// task switch and an allocation per frame, and the work under these locks is a few hundred nanoseconds.
public final class Locked<Value>: @unchecked Sendable {
    private var value: Value
    private let lock = OSAllocatedUnfairLock()

    public init(_ value: Value) { self.value = value }

    @inline(__always)
    public func withLock<R>(_ body: (inout Value) throws -> R) rethrows -> R {
        lock.lock()
        defer { lock.unlock() }
        return try body(&value)
    }

    public var snapshot: Value { withLock { $0 } }
}

/// Unchecked sendable wrapper for CoreFoundation / CoreVideo objects that are immutable once created
/// or are only touched under an external serial queue (CVPixelBuffer, CMSampleBuffer, CMBlockBuffer).
public struct UncheckedSendable<T>: @unchecked Sendable {
    public var value: T
    public init(_ value: T) { self.value = value }
}

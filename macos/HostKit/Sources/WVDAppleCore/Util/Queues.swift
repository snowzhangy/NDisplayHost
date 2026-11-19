import Foundation

/// Fixed-capacity FIFO. Every queue in the pipeline is bounded (plan §6.6) and reports its depth.
public struct BoundedQueue<Element> {
    public enum OverflowPolicy: Sendable { case dropOldest, rejectNewest }

    public let capacity: Int
    public let policy: OverflowPolicy
    private var storage: [Element?]
    private var head = 0
    public private(set) var count = 0
    public private(set) var dropped: UInt64 = 0
    public private(set) var peak = 0

    public init(capacity: Int, policy: OverflowPolicy = .dropOldest) {
        precondition(capacity > 0)
        self.capacity = capacity
        self.policy = policy
        self.storage = Array(repeating: nil, count: capacity)
    }

    public var isEmpty: Bool { count == 0 }

    /// Returns the element that was evicted or rejected, if any.
    @discardableResult
    public mutating func push(_ element: Element) -> Element? {
        if count == capacity {
            dropped += 1
            switch policy {
            case .rejectNewest:
                return element
            case .dropOldest:
                let evicted = storage[head]
                storage[head] = element
                head = (head + 1) % capacity
                return evicted
            }
        }
        storage[(head + count) % capacity] = element
        count += 1
        peak = max(peak, count)
        return nil
    }

    public mutating func pop() -> Element? {
        guard count > 0 else { return nil }
        let element = storage[head]
        storage[head] = nil
        head = (head + 1) % capacity
        count -= 1
        return element
    }

    public mutating func removeAll() {
        while pop() != nil {}
    }

    public var depth: QueueDepthSnapshot { QueueDepthSnapshot(depth: count, capacity: capacity, peak: peak, dropped: dropped) }
}

public struct QueueDepthSnapshot: Sendable, Equatable {
    public var depth: Int
    public var capacity: Int
    public var peak: Int
    public var dropped: UInt64

    public init(depth: Int, capacity: Int, peak: Int, dropped: UInt64) {
        self.depth = depth
        self.capacity = capacity
        self.peak = peak
        self.dropped = dropped
    }
}

/// Thread-safe single-slot newest-wins mailbox (capture and presentation stages, plan §6.6).
///
/// `put` returns `true` when the consumer needs to be woken (the slot was empty and no drain is
/// scheduled), so producers schedule at most one consumer hop however fast they produce.
public final class NewestWinsMailbox<T>: @unchecked Sendable {
    private struct State {
        var slot: T?
        var drainScheduled = false
        var superseded: UInt64 = 0
        var delivered: UInt64 = 0
    }
    private let state = Locked(State())

    public init() {}

    public func put(_ value: T) -> Bool {
        state.withLock { s in
            if s.slot != nil { s.superseded += 1 }
            s.slot = value
            if s.drainScheduled { return false }
            s.drainScheduled = true
            return true
        }
    }

    /// Takes the newest value and clears the scheduled flag.
    public func take() -> T? {
        state.withLock { s in
            s.drainScheduled = false
            let v = s.slot
            s.slot = nil
            if v != nil { s.delivered += 1 }
            return v
        }
    }

    /// Takes the newest value without touching the scheduling flag (polling consumers).
    public func poll() -> T? {
        state.withLock { s in
            let v = s.slot
            s.slot = nil
            if v != nil { s.delivered += 1 }
            return v
        }
    }

    public var hasValue: Bool { state.withLock { $0.slot != nil } }

    public var counters: (superseded: UInt64, delivered: UInt64) {
        state.withLock { ($0.superseded, $0.delivered) }
    }
}

/// Rolling sample window for p50/p95/p99 reporting (plan §8 reports percentiles, never means).
public struct PercentileWindow: Sendable {
    private var samples: [UInt32]
    private var next = 0
    private var filled = 0

    public init(capacity: Int = 256) {
        samples = Array(repeating: 0, count: capacity)
    }

    public mutating func add(_ value: UInt32) {
        samples[next] = value
        next = (next + 1) % samples.count
        filled = min(filled + 1, samples.count)
    }

    public mutating func add(micros value: UInt64) { add(UInt32(clamping: value)) }

    public var count: Int { filled }

    public func percentile(_ p: Double) -> UInt32 {
        guard filled > 0 else { return 0 }
        var sorted = Array(samples[0..<filled])
        sorted.sort()
        let idx = min(filled - 1, max(0, Int((Double(filled - 1) * p).rounded())))
        return sorted[idx]
    }

    public var p50: UInt32 { percentile(0.50) }
    public var p95: UInt32 { percentile(0.95) }
    public var p99: UInt32 { percentile(0.99) }

    public mutating func reset() {
        next = 0
        filled = 0
    }
}

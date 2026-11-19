#if os(macOS)
import Foundation
import Network
import WVDAppleCore

/// Paces media datagrams onto the wire.
///
/// Network.framework drops QUIC datagrams it cannot send immediately, so an unpaced 4K IDR (hundreds of
/// datagrams) loses most of itself (measured on loopback during bring-up: 2000-datagram bursts lost ~60%,
/// the same volume paced at ~325 Mbit/s arrived intact). Each picture is spread over at most
/// `spreadFraction` of a frame interval, never faster than `maxRateBps`.
public final class Pacer: @unchecked Sendable {
    public struct Config: Sendable {
        public var maxRateBps: Double = 600_000_000
        public var minRateBps: Double = 60_000_000
        public var spreadFraction: Double = 0.5
        public var quantumMicros: UInt64 = 250
        public var maxQueuedBytes = 8 << 20
        public init() {}
    }

    private struct Item {
        var datagram: Data
        var rateBps: Double
    }

    private let send: @Sendable ([Data]) -> Void
    private let config: Config
    private let lock = NSCondition()
    private var queue: [Item] = []
    private var head = 0
    private var queuedBytes = 0
    private var running = true
    private var thread: Thread?
    private(set) var sentBytes: UInt64 = 0
    private(set) var droppedBytes: UInt64 = 0

    public init(config: Config = Config(), send: @escaping @Sendable ([Data]) -> Void) {
        self.config = config
        self.send = send
        let t = Thread { [weak self] in self?.run() }
        t.qualityOfService = .userInteractive
        t.name = "wvd.pacer"
        thread = t
        t.start()
    }

    deinit { stop() }

    public func stop() {
        lock.lock()
        running = false
        lock.signal()
        lock.unlock()
    }

    /// Queues one picture's datagrams. `frameIntervalMicros` sets the spread; `avgBitrate` sets the floor.
    public func enqueue(_ datagrams: [Data], frameIntervalMicros: UInt64, avgBitrateBps: Double) {
        guard !datagrams.isEmpty else { return }
        let bytes = datagrams.reduce(0) { $0 + $1.count }
        let spread = max(Double(frameIntervalMicros) * config.spreadFraction, 1_000) / 1_000_000
        let rate = min(config.maxRateBps, max(config.minRateBps, Double(bytes * 8) / spread, avgBitrateBps * 2))
        lock.lock()
        if queuedBytes + bytes > config.maxQueuedBytes {
            // Bounded (plan §6.6). The receiver detects the gap and requests recovery.
            droppedBytes += UInt64(bytes)
            lock.unlock()
            return
        }
        for d in datagrams { queue.append(Item(datagram: d, rateBps: rate)) }
        queuedBytes += bytes
        lock.signal()
        lock.unlock()
    }

    /// Urgent datagrams (cursor) bypass the queue.
    public func sendNow(_ datagram: Data) { send([datagram]) }

    public var backlogBytes: Int {
        lock.lock(); defer { lock.unlock() }
        return queuedBytes
    }

    private func run() {
        var budget: Double = 0  // bytes
        var last = MonoClock.nowNanos()
        while true {
            lock.lock()
            while running && head >= queue.count { lock.wait() }
            if !running { lock.unlock(); return }
            let now = MonoClock.nowNanos()
            let rate = queue[head].rateBps
            budget = min(budget + Double(now - last) / 1e9 * rate / 8, rate / 8 * Double(config.quantumMicros) * 4 / 1e6 + 1500)
            last = now
            var batch: [Data] = []
            while head < queue.count, budget >= Double(queue[head].datagram.count) || batch.isEmpty && budget > 0 {
                let d = queue[head].datagram
                budget -= Double(d.count)
                batch.append(d)
                head += 1
                queuedBytes -= d.count
            }
            if head > 1024 && head * 2 > queue.count {
                queue.removeFirst(head)
                head = 0
            } else if head >= queue.count {
                queue.removeAll(keepingCapacity: true)
                head = 0
            }
            lock.unlock()
            if !batch.isEmpty {
                send(batch)
                sentBytes += UInt64(batch.reduce(0) { $0 + $1.count })
            }
            MonoClock.sleep(untilNanos: MonoClock.nowNanos() + config.quantumMicros * 1_000)
        }
    }
}
#endif

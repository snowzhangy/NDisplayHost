import Foundation

public struct ReassemblerConfig: Sendable, Equatable {
    /// Bounded: at most this many pictures in reassembly (plan §6.6). Overflow declares the oldest lost.
    public var maxInFlight: Int = 8
    /// An incomplete picture is abandoned this long after its first shard (or after a later picture
    /// showed it was skipped). Tuned from RTT by the receiver; never longer than the deadline budget.
    public var lossTimeoutMicros: UInt64 = 40_000
    /// Quiet time after the last shard of a picture before it is considered fully sent.
    public var sendCompleteMicros: UInt64 = 2_000
    /// Wait between NACK retries.
    public var nackRetryMicros: UInt64 = 8_000
    public var maxNacksPerFrame: Int = 2
    public var nackEnabled = true

    public init() {}
}

public struct CompletedFrame: @unchecked Sendable {
    public var header: MediaShardHeader
    public var buffer: AUBuffer
    public var firstArrivalMicros: UInt64
    public var completeMicros: UInt64
    public var randomAccess: Bool { header.flags.contains(.randomAccess) }
    public var refinement: Bool { header.flags.contains(.refinement) }

    public init(header: MediaShardHeader, buffer: AUBuffer, firstArrivalMicros: UInt64, completeMicros: UInt64) {
        self.header = header
        self.buffer = buffer
        self.firstArrivalMicros = firstArrivalMicros
        self.completeMicros = completeMicros
    }
}

public enum ReassemblyEvent: @unchecked Sendable {
    /// Next picture in order, complete.
    case frame(CompletedFrame)
    /// Picture could not be reassembled. `supersededByRandomAccess` = a random-access picture follows
    /// immediately, so the decoder needs no recovery request.
    case lost(picSeq: UInt32, codecGen: UInt32, supersededByRandomAccess: Bool)
    case nack(NackDatagram)
}

public struct ReassemblyCounters: Sendable, Equatable {
    public var shardsReceived: UInt64 = 0
    public var duplicateShards: UInt64 = 0
    public var lateShards: UInt64 = 0
    public var malformedShards: UInt64 = 0
    public var shardsRecovered: UInt64 = 0
    public var shardsMissing: UInt64 = 0
    public var framesCompleted: UInt64 = 0
    public var framesLost: UInt64 = 0
    public var nacksSent: UInt64 = 0
    public var bytesReceived: UInt64 = 0

    public init() {}
}

/// Turns media shards back into pictures, in `pic_seq` order. Owned by one serial queue.
public final class Reassembler {
    private final class Assembly {
        struct Block {
            var k: Int
            var m: Int
            var parity: [Int: [UInt8]] = [:]
            var done = false
        }

        let header: MediaShardHeader
        let totalData: Int
        let shardBytes: Int
        let buffer: AUBuffer
        var have: [Bool]
        var haveCount = 0
        var blocks: [Int: Block] = [:]
        let firstArrival: UInt64
        var lastArrival: UInt64
        var nacks = 0
        var lastNack: UInt64 = 0

        init(header h: MediaShardHeader, now: UInt64) {
            header = h
            totalData = Int(h.totalDataShards)
            shardBytes = Int(h.shardBytes)
            buffer = AUBuffer(capacity: totalData * shardBytes)
            buffer.length = Int(h.frameBytes)
            have = Array(repeating: false, count: totalData)
            firstArrival = now
            lastArrival = now
        }

        var complete: Bool { haveCount == totalData }

        func matches(_ h: MediaShardHeader) -> Bool {
            h.codecGen == header.codecGen && h.frameBytes == header.frameBytes && h.shardBytes == header.shardBytes
                && h.totalDataShards == header.totalDataShards
        }

        func missingData(limit: Int) -> [UInt16] {
            var out: [UInt16] = []
            for i in 0..<totalData where !have[i] {
                out.append(UInt16(i))
                if out.count == limit { break }
            }
            return out
        }
    }

    public var config: ReassemblerConfig
    public private(set) var counters = ReassemblyCounters()

    private var assemblies: [UInt32: Assembly] = [:]
    /// Pictures we know exist (a later pic_seq arrived) but have no shard of yet: first-noticed time.
    private var phantoms: [UInt32: (noticed: UInt64, nacks: Int, lastNack: UInt64)] = [:]
    private var nextExpected: UInt32?
    private var highestSeen: UInt32?
    private var lastCodecGen: UInt32 = 0
    private var lastMonitorID: UInt16 = 0

    public init(config: ReassemblerConfig = ReassemblerConfig()) {
        self.config = config
    }

    public var inFlight: Int { assemblies.count }
    public var depth: QueueDepthSnapshot { QueueDepthSnapshot(depth: assemblies.count, capacity: config.maxInFlight, peak: 0, dropped: counters.framesLost) }

    /// Drops all state; the next delivered picture must be a random-access picture.
    public func reset() {
        assemblies.removeAll()
        phantoms.removeAll()
        nextExpected = nil
        highestSeen = nil
    }

    public func insert(_ datagram: UnsafeRawBufferPointer, now: UInt64) -> [ReassemblyEvent] {
        guard let h = MediaShardHeader.parse(datagram) else {
            counters.malformedShards += 1
            return []
        }
        counters.shardsReceived += 1
        counters.bytesReceived += UInt64(datagram.count)
        lastCodecGen = h.codecGen
        lastMonitorID = h.monitorID
        let seq = h.picSeq

        if let next = nextExpected, seq < next {
            counters.lateShards += 1
            return []
        }
        var events: [ReassemblyEvent] = []

        let assembly: Assembly
        if let existing = assemblies[seq] {
            guard existing.matches(h) else {
                counters.malformedShards += 1
                return []
            }
            assembly = existing
        } else {
            while assemblies.count >= config.maxInFlight, let oldest = assemblies.keys.min() {
                events.append(contentsOf: abandon(oldest, now: now))
            }
            if let next = nextExpected, seq < next { return events }  // abandoned while making room
            assembly = Assembly(header: h, now: now)
            assemblies[seq] = assembly
            phantoms[seq] = nil
            if nextExpected != nil, let hi = highestSeen, seq > hi + 1 {
                // Pictures between hi and seq have not produced a single shard yet.
                let start = max(hi + 1, nextExpected ?? hi + 1)
                if seq > start {
                    for missing in start..<seq where assemblies[missing] == nil && phantoms[missing] == nil {
                        phantoms[missing] = (now, 0, 0)
                    }
                }
            }
        }
        highestSeen = max(highestSeen ?? seq, seq)
        assembly.lastArrival = now

        let payload = UnsafeRawBufferPointer(rebasing: datagram[MediaShardHeader.size..<MediaShardHeader.size + Int(h.shardBytes)])
        let first = Int(h.blockFirstShard)
        let k = Int(h.dataShards)
        if assembly.blocks[first] == nil {
            assembly.blocks[first] = Assembly.Block(k: k, m: Int(h.parityShards))
        }
        if h.isParity {
            if var block = assembly.blocks[first], !block.done {
                let row = Int(h.shardIndex) - k
                if block.parity[row] == nil {
                    block.parity[row] = Array(payload)
                    assembly.blocks[first] = block
                } else {
                    counters.duplicateShards += 1
                }
            } else {
                counters.duplicateShards += 1
            }
        } else {
            let global = first + Int(h.shardIndex)
            if assembly.have[global] {
                counters.duplicateShards += 1
            } else {
                (assembly.buffer.pointer + global * assembly.shardBytes).copyMemory(from: payload.baseAddress!, byteCount: assembly.shardBytes)
                assembly.have[global] = true
                assembly.haveCount += 1
            }
        }
        tryRecover(assembly, blockFirst: first)
        events.append(contentsOf: drain(now: now))
        return events
    }

    /// Runs time-based NACK and loss decisions. Call every few milliseconds and after inserts.
    public func sweep(now: UInt64) -> [ReassemblyEvent] {
        var events = drain(now: now)
        guard config.nackEnabled else { return events }
        for (seq, a) in assemblies where !a.complete && (nextExpected != nil || a.header.flags.contains(.randomAccess)) {
            let laterSeen = (highestSeen ?? seq) > seq
            let sendFinished = laterSeen || now - a.lastArrival >= config.sendCompleteMicros
            guard sendFinished, a.nacks < config.maxNacksPerFrame,
                  a.nacks == 0 || now - a.lastNack >= config.nackRetryMicros else { continue }
            guard !recoverableWithoutNack(a) else { continue }
            let missing = a.missingData(limit: NackDatagram.maxIndices)
            guard !missing.isEmpty else { continue }
            a.nacks += 1
            a.lastNack = now
            counters.nacksSent += 1
            events.append(.nack(NackDatagram(monitorID: a.header.monitorID, codecGen: a.header.codecGen, picSeq: seq, missing: missing)))
        }
        for (seq, p) in phantoms where p.nacks < config.maxNacksPerFrame && (p.nacks == 0 || now - p.lastNack >= config.nackRetryMicros) {
            guard now - p.noticed >= config.sendCompleteMicros else { continue }
            phantoms[seq] = (p.noticed, p.nacks + 1, now)
            counters.nacksSent += 1
            events.append(.nack(NackDatagram(monitorID: lastMonitorID, codecGen: lastCodecGen, picSeq: seq, missing: [])))
        }
        return events
    }

    private func recoverableWithoutNack(_ a: Assembly) -> Bool {
        // Every block with missing data must already hold enough parity.
        for (first, b) in a.blocks where !b.done {
            let missing = (first..<first + b.k).reduce(0) { $0 + (a.have[$1] ? 0 : 1) }
            if missing > b.parity.count { return false }
        }
        // Blocks never seen at all are unrecoverable without NACK.
        var covered = 0
        for b in a.blocks.values { covered += b.k }
        return covered == a.totalData
    }

    private func tryRecover(_ a: Assembly, blockFirst first: Int) {
        guard var block = a.blocks[first], !block.done else { return }
        let range = first..<first + block.k
        let present = range.map { a.have[$0] }
        let missingCount = present.reduce(0) { $0 + ($1 ? 0 : 1) }
        if missingCount == 0 {
            block.done = true
            block.parity.removeAll()
            a.blocks[first] = block
            return
        }
        guard block.parity.count >= missingCount else { return }
        let s = a.shardBytes
        let base = a.buffer.pointer.assumingMemoryBound(to: UInt8.self)
        let dataPtrs = range.map { base + $0 * s }
        let rows = block.parity.keys.sorted()
        let scratch = UnsafeMutablePointer<UInt8>.allocate(capacity: rows.count * s)
        defer { scratch.deallocate() }
        for (i, r) in rows.enumerated() {
            block.parity[r]!.withUnsafeBufferPointer { (scratch + i * s).update(from: $0.baseAddress!, count: s) }
        }
        let parityPtrs = rows.enumerated().map { (row: $0.element, shard: UnsafePointer(scratch + $0.offset * s)) }
        let ok = (try? ReedSolomon.reconstruct(k: block.k, m: block.m, data: dataPtrs, present: present,
                                               parity: parityPtrs, shardBytes: s)) != nil
        guard ok else { return }
        for i in range where !a.have[i] {
            a.have[i] = true
            a.haveCount += 1
        }
        counters.shardsRecovered += UInt64(missingCount)
        block.done = true
        block.parity.removeAll()
        a.blocks[first] = block
    }

    private func deliver(_ seq: UInt32, _ a: Assembly, now: UInt64) -> ReassemblyEvent {
        assemblies[seq] = nil
        counters.framesCompleted += 1
        var header = a.header
        header.flags.remove(.parity)
        header.flags.remove(.retransmit)
        return .frame(CompletedFrame(header: header, buffer: a.buffer, firstArrivalMicros: a.firstArrival, completeMicros: now))
    }

    private func abandon(_ seq: UInt32, now: UInt64) -> [ReassemblyEvent] {
        if let a = assemblies.removeValue(forKey: seq) {
            counters.shardsMissing += UInt64(a.totalData - a.haveCount)
        }
        phantoms[seq] = nil
        counters.framesLost += 1
        if let next = nextExpected, seq >= next { nextExpected = seq + 1 }
        let ra = nextRandomAccessComplete(after: seq) == seq + 1
        return [.lost(picSeq: seq, codecGen: lastCodecGen, supersededByRandomAccess: ra)]
    }

    private func firstRandomAccessComplete() -> UInt32? {
        assemblies.filter { $0.value.complete && $0.value.header.flags.contains(.randomAccess) }.keys.min()
    }

    private func nextRandomAccessComplete(after seq: UInt32) -> UInt32? {
        assemblies.filter { $0.key > seq && $0.value.complete && $0.value.header.flags.contains(.randomAccess) }.keys.min()
    }

    private func drain(now: UInt64) -> [ReassemblyEvent] {
        var events: [ReassemblyEvent] = []
        if nextExpected == nil {
            // Start of stream / after reset: the first picture delivered must be random access.
            guard let ra = firstRandomAccessComplete(), let a = assemblies[ra] else {
                // Discard complete non-random-access pictures; they can never be decoded.
                for (seq, a) in assemblies where a.complete && !a.header.flags.contains(.randomAccess) {
                    assemblies[seq] = nil
                }
                return events
            }
            for seq in assemblies.keys where seq < ra { assemblies[seq] = nil }
            phantoms = phantoms.filter { $0.key > ra }
            events.append(deliver(ra, a, now: now))
            nextExpected = ra + 1
        }
        while let next = nextExpected {
            if let a = assemblies[next] {
                if a.complete {
                    events.append(deliver(next, a, now: now))
                    nextExpected = next + 1
                    continue
                }
                if let ra = nextRandomAccessComplete(after: next) {
                    // A later random-access picture makes everything before it irrelevant.
                    for seq in next..<ra { events.append(contentsOf: abandonQuietly(seq)) }
                    nextExpected = ra
                    continue
                }
                if now - a.firstArrival >= config.lossTimeoutMicros {
                    events.append(contentsOf: abandon(next, now: now))
                    continue
                }
                break
            } else if let p = phantoms[next] {
                if let ra = nextRandomAccessComplete(after: next) {
                    for seq in next..<ra { events.append(contentsOf: abandonQuietly(seq)) }
                    nextExpected = ra
                    continue
                }
                if now - p.noticed >= config.lossTimeoutMicros {
                    events.append(contentsOf: abandon(next, now: now))
                    continue
                }
                break
            } else {
                break  // nothing known about `next` yet
            }
        }
        return events
    }

    /// Abandons a picture that a following random-access picture supersedes.
    private func abandonQuietly(_ seq: UInt32) -> [ReassemblyEvent] {
        let existed = assemblies[seq] != nil || phantoms[seq] != nil
        if let a = assemblies.removeValue(forKey: seq) { counters.shardsMissing += UInt64(a.totalData - a.haveCount) }
        phantoms[seq] = nil
        guard existed else { return [] }
        counters.framesLost += 1
        return [.lost(picSeq: seq, codecGen: lastCodecGen, supersededByRandomAccess: true)]
    }
}

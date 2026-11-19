import Foundation

/// Owned, 16-byte-aligned byte buffer for one access unit. Reassembly writes shards straight into it and
/// the decoder wraps it in a CMBlockBuffer without copying.
public final class AUBuffer: @unchecked Sendable {
    public let pointer: UnsafeMutableRawPointer
    public let capacity: Int
    public internal(set) var length: Int

    public init(capacity: Int) {
        self.capacity = max(capacity, 1)
        self.pointer = UnsafeMutableRawPointer.allocate(byteCount: self.capacity, alignment: 16)
        self.length = 0
    }

    public convenience init(copying bytes: UnsafeRawBufferPointer) {
        self.init(capacity: bytes.count)
        if let base = bytes.baseAddress { pointer.copyMemory(from: base, byteCount: bytes.count) }
        length = bytes.count
    }

    deinit { pointer.deallocate() }

    public var bytes: UnsafeRawBufferPointer { UnsafeRawBufferPointer(start: pointer, count: length) }
}

public struct PacketizerConfig: Sendable, Equatable {
    /// Payload bytes per shard. Header (48) + payload must fit the path's usable datagram size.
    public var shardBytes: Int
    /// Data shards per Reed-Solomon block. Smaller blocks cut FEC CPU (cost ~ parity x frame bytes) and,
    /// with interleaving, spread a Wi-Fi loss burst across blocks.
    public var maxBlockDataShards: Int
    /// Parity shards per data shard, adapted by the host from client loss reports.
    public var fecRatio: Double
    public var minParityPerBlock: Int
    public var maxParityPerBlock: Int

    public init(shardBytes: Int = 1152, maxBlockDataShards: Int = 48, fecRatio: Double = 0.10,
                minParityPerBlock: Int = 1, maxParityPerBlock: Int = 24) {
        self.shardBytes = shardBytes; self.maxBlockDataShards = maxBlockDataShards; self.fecRatio = fecRatio
        self.minParityPerBlock = minParityPerBlock; self.maxParityPerBlock = maxParityPerBlock
    }

    public static func forDatagramSize(_ usable: Int) -> PacketizerConfig {
        var c = PacketizerConfig()
        c.shardBytes = max(256, min(usable, 1400) - MediaShardHeader.size)
        return c
    }
}

/// Everything about a coded picture except its bytes (plan §6.6 identifiers).
public struct FrameDescriptor: Sendable, Equatable {
    public var monitorID: UInt16
    public var sessionGen: UInt32
    public var layoutGen: UInt32
    public var codecGen: UInt32
    public var picSeq: UInt32
    public var contentVer: UInt32
    public var randomAccess: Bool
    public var refinement: Bool
    public var captureTimeMicros: UInt64
    public var deadlineBudgetMillis: UInt16

    public init(monitorID: UInt16, sessionGen: UInt32, layoutGen: UInt32, codecGen: UInt32, picSeq: UInt32,
                contentVer: UInt32, randomAccess: Bool, refinement: Bool, captureTimeMicros: UInt64, deadlineBudgetMillis: UInt16) {
        self.monitorID = monitorID; self.sessionGen = sessionGen; self.layoutGen = layoutGen; self.codecGen = codecGen
        self.picSeq = picSeq; self.contentVer = contentVer; self.randomAccess = randomAccess; self.refinement = refinement
        self.captureTimeMicros = captureTimeMicros; self.deadlineBudgetMillis = deadlineBudgetMillis
    }
}

/// A picture split into datagrams. Data datagrams can be sent while parity is still being computed.
public final class PacketizedFrame: @unchecked Sendable {
    public struct Block: Sendable, Equatable {
        public var firstShard: Int
        public var k: Int
        public var m: Int
    }

    public let descriptor: FrameDescriptor
    public let frameBytes: Int
    public let shardBytes: Int
    public let totalDataShards: Int
    public let blocks: [Block]
    /// Data datagrams by global data-shard index.
    public let dataDatagrams: [Data]
    /// Parity datagrams per block, row order. Empty until `computeParity()`.
    public private(set) var parityDatagrams: [[Data]] = []
    private var padded: [UInt8]

    init(descriptor: FrameDescriptor, au: UnsafeRawBufferPointer, config: PacketizerConfig) {
        self.descriptor = descriptor
        self.frameBytes = au.count
        let s = config.shardBytes
        self.shardBytes = s
        let n = max(1, (au.count + s - 1) / s)
        precondition(n <= Int(UInt16.max), "access unit too large for one picture")
        self.totalDataShards = n

        let blockCount = (n + config.maxBlockDataShards - 1) / config.maxBlockDataShards
        let base = n / blockCount, extra = n % blockCount
        var blocks: [Block] = []
        var first = 0
        for b in 0..<blockCount {
            let k = base + (b < extra ? 1 : 0)
            var m = Int((Double(k) * config.fecRatio).rounded(.up))
            m = min(max(m, config.minParityPerBlock), config.maxParityPerBlock, 255 - k)
            if config.fecRatio <= 0 { m = 0 }
            blocks.append(Block(firstShard: first, k: k, m: m))
            first += k
        }
        self.blocks = blocks

        padded = [UInt8](repeating: 0, count: n * s)
        padded.withUnsafeMutableBytes { dst in
            if let src = au.baseAddress { dst.baseAddress!.copyMemory(from: src, byteCount: au.count) }
        }

        var datagrams: [Data] = []
        datagrams.reserveCapacity(n)
        var flags: MediaShardHeader.Flags = []
        if descriptor.randomAccess { flags.insert(.randomAccess) }
        if descriptor.refinement { flags.insert(.refinement) }
        padded.withUnsafeBytes { src in
            for block in blocks {
                for i in 0..<block.k {
                    let global = block.firstShard + i
                    var d = Data(count: MediaShardHeader.size + s)
                    d.withUnsafeMutableBytes { raw in
                        let p = raw.baseAddress!
                        PacketizedFrame.header(descriptor, flags: flags, frameBytes: au.count, block: block, index: i,
                                               shardBytes: s, total: n).write(to: p)
                        (p + MediaShardHeader.size).copyMemory(from: src.baseAddress! + global * s, byteCount: s)
                    }
                    datagrams.append(d)
                }
            }
        }
        self.dataDatagrams = datagrams
    }

    static func header(_ d: FrameDescriptor, flags: MediaShardHeader.Flags, frameBytes: Int, block: Block, index: Int,
                       shardBytes: Int, total: Int) -> MediaShardHeader {
        MediaShardHeader(flags: flags, monitorID: d.monitorID, sessionGen: d.sessionGen, layoutGen: d.layoutGen,
                         codecGen: d.codecGen, picSeq: d.picSeq, contentVer: d.contentVer, frameBytes: UInt32(frameBytes),
                         blockFirstShard: UInt16(block.firstShard), shardIndex: UInt16(index), dataShards: UInt8(block.k),
                         parityShards: UInt8(block.m), shardBytes: UInt16(shardBytes), totalDataShards: UInt16(total),
                         deadlineBudgetMillis: d.deadlineBudgetMillis, captureTimeMicros: d.captureTimeMicros)
    }

    /// Data datagrams interleaved across blocks (block0#0, block1#0, ..., block0#1, ...), so a burst
    /// loss costs each block at most a few shards.
    public func interleavedData() -> [Data] {
        interleave(blocks.map { b in Array(dataDatagrams[b.firstShard..<b.firstShard + b.k]) })
    }

    public func interleavedParity() -> [Data] { interleave(parityDatagrams) }

    private func interleave(_ lists: [[Data]]) -> [Data] {
        var out: [Data] = []
        out.reserveCapacity(lists.reduce(0) { $0 + $1.count })
        let longest = lists.map(\.count).max() ?? 0
        for i in 0..<longest {
            for l in lists where i < l.count { out.append(l[i]) }
        }
        return out
    }

    /// Computes Reed-Solomon parity for every block. Idempotent.
    public func computeParity() {
        guard parityDatagrams.isEmpty else { return }
        let s = shardBytes
        var flags: MediaShardHeader.Flags = [.parity]
        if descriptor.randomAccess { flags.insert(.randomAccess) }
        if descriptor.refinement { flags.insert(.refinement) }
        var result: [[Data]] = []
        result.reserveCapacity(blocks.count)
        padded.withUnsafeBufferPointer { src in
            for block in blocks {
                guard block.m > 0 else { result.append([]); continue }
                let scratch = UnsafeMutablePointer<UInt8>.allocate(capacity: block.m * s)
                defer { scratch.deallocate() }
                let data = (0..<block.k).map { src.baseAddress! + (block.firstShard + $0) * s }
                let parity = (0..<block.m).map { scratch + $0 * s }
                try? ReedSolomon.encode(data: data, parity: parity, shardBytes: s)
                var list: [Data] = []
                for j in 0..<block.m {
                    var d = Data(count: MediaShardHeader.size + s)
                    d.withUnsafeMutableBytes { raw in
                        let p = raw.baseAddress!
                        PacketizedFrame.header(descriptor, flags: flags, frameBytes: frameBytes, block: block,
                                               index: block.k + j, shardBytes: s, total: totalDataShards).write(to: p)
                        (p + MediaShardHeader.size).copyMemory(from: scratch + j * s, byteCount: s)
                    }
                    list.append(d)
                }
                result.append(list)
            }
        }
        parityDatagrams = result
    }

    /// Copies of the requested data shards with the RETRANSMIT flag set. Empty request = whole picture.
    public func retransmission(of indices: [UInt16]) -> [Data] {
        let wanted = indices.isEmpty ? Array(0..<totalDataShards) : indices.map(Int.init).filter { $0 < totalDataShards }
        return wanted.map { i in
            var d = dataDatagrams[i]
            d.withUnsafeMutableBytes { raw in
                raw[1] |= MediaShardHeader.Flags.retransmit.rawValue
            }
            return d
        }
    }

    public var totalDatagramBytes: Int {
        dataDatagrams.count * (MediaShardHeader.size + shardBytes) + parityDatagrams.reduce(0) { $0 + $1.count } * (MediaShardHeader.size + shardBytes)
    }
}

public enum Packetizer {
    /// Splits an access unit into data datagrams. Call `computeParity()` on the result afterwards
    /// (ideally while the data datagrams are already being paced out).
    public static func packetize(_ au: UnsafeRawBufferPointer, descriptor: FrameDescriptor, config: PacketizerConfig) -> PacketizedFrame {
        PacketizedFrame(descriptor: descriptor, au: au, config: config)
    }
}

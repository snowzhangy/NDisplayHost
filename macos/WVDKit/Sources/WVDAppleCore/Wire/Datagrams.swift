import Foundation

// Fixed little-endian QUIC datagram layouts (core/proto/WIRE.md §4). Hot path: no varints, no allocation
// to parse, payload addressed in place.

public enum DatagramType: UInt8, Sendable {
    case mediaShard = 0x01    // host -> client
    case cursor = 0x02        // host -> client
    case audio = 0x03         // host -> client, see AudioPacket
    case flowHello = 0x10     // client -> host: binds the datagram flow to the session
    case nack = 0x11          // client -> host: retransmit request
}

@inline(__always) func loadLE<T: FixedWidthInteger>(_ p: UnsafeRawPointer, _ offset: Int, as: T.Type) -> T {
    T(littleEndian: p.loadUnaligned(fromByteOffset: offset, as: T.self))
}

@inline(__always) func storeLE<T: FixedWidthInteger>(_ p: UnsafeMutableRawPointer, _ offset: Int, _ value: T) {
    p.storeBytes(of: value.littleEndian, toByteOffset: offset, as: T.self)
}

/// Header of one media shard. A coded picture ("access unit", AU) is split into `totalDataShards`
/// equal-size data shards of `shardBytes`; shards are grouped into Reed-Solomon blocks of `k` data +
/// `m` parity shards, identified by the global index of their first data shard.
public struct MediaShardHeader: Sendable, Equatable {
    public static let size = 48

    public struct Flags: OptionSet, Sendable, Hashable {
        public let rawValue: UInt8
        public init(rawValue: UInt8) { self.rawValue = rawValue }
        public static let randomAccess = Flags(rawValue: 1 << 0)  // IDR / CRA: decodable on its own
        public static let parity = Flags(rawValue: 1 << 1)        // this shard is RS parity
        public static let refinement = Flags(rawValue: 1 << 2)    // static-content quality refinement
        public static let retransmit = Flags(rawValue: 1 << 3)    // NACK response
    }

    public var flags: Flags
    public var monitorID: UInt16
    public var sessionGen: UInt32
    public var layoutGen: UInt32
    public var codecGen: UInt32
    public var picSeq: UInt32
    public var contentVer: UInt32
    public var frameBytes: UInt32
    public var blockFirstShard: UInt16
    public var shardIndex: UInt16       // within the block: [0, k) data, [k, k+m) parity
    public var dataShards: UInt8        // k
    public var parityShards: UInt8      // m
    public var shardBytes: UInt16
    public var totalDataShards: UInt16  // N for the whole picture
    public var deadlineBudgetMillis: UInt16
    public var captureTimeMicros: UInt64  // host clock

    public init(flags: Flags, monitorID: UInt16, sessionGen: UInt32, layoutGen: UInt32, codecGen: UInt32,
                picSeq: UInt32, contentVer: UInt32, frameBytes: UInt32, blockFirstShard: UInt16, shardIndex: UInt16,
                dataShards: UInt8, parityShards: UInt8, shardBytes: UInt16, totalDataShards: UInt16,
                deadlineBudgetMillis: UInt16, captureTimeMicros: UInt64) {
        self.flags = flags; self.monitorID = monitorID; self.sessionGen = sessionGen; self.layoutGen = layoutGen
        self.codecGen = codecGen; self.picSeq = picSeq; self.contentVer = contentVer; self.frameBytes = frameBytes
        self.blockFirstShard = blockFirstShard; self.shardIndex = shardIndex; self.dataShards = dataShards
        self.parityShards = parityShards; self.shardBytes = shardBytes; self.totalDataShards = totalDataShards
        self.deadlineBudgetMillis = deadlineBudgetMillis; self.captureTimeMicros = captureTimeMicros
    }

    public func write(to p: UnsafeMutableRawPointer) {
        p.storeBytes(of: DatagramType.mediaShard.rawValue, as: UInt8.self)
        p.storeBytes(of: flags.rawValue, toByteOffset: 1, as: UInt8.self)
        storeLE(p, 2, monitorID)
        storeLE(p, 4, sessionGen)
        storeLE(p, 8, layoutGen)
        storeLE(p, 12, codecGen)
        storeLE(p, 16, picSeq)
        storeLE(p, 20, contentVer)
        storeLE(p, 24, frameBytes)
        storeLE(p, 28, blockFirstShard)
        storeLE(p, 30, shardIndex)
        p.storeBytes(of: dataShards, toByteOffset: 32, as: UInt8.self)
        p.storeBytes(of: parityShards, toByteOffset: 33, as: UInt8.self)
        storeLE(p, 34, shardBytes)
        storeLE(p, 36, totalDataShards)
        storeLE(p, 38, deadlineBudgetMillis)
        storeLE(p, 40, captureTimeMicros)
    }

    /// Parses and validates; `nil` for anything malformed (never trust the network).
    public static func parse(_ p: UnsafeRawBufferPointer) -> MediaShardHeader? {
        guard p.count >= size, let base = p.baseAddress, p[0] == DatagramType.mediaShard.rawValue else { return nil }
        let h = MediaShardHeader(
            flags: Flags(rawValue: p[1]),
            monitorID: loadLE(base, 2, as: UInt16.self),
            sessionGen: loadLE(base, 4, as: UInt32.self),
            layoutGen: loadLE(base, 8, as: UInt32.self),
            codecGen: loadLE(base, 12, as: UInt32.self),
            picSeq: loadLE(base, 16, as: UInt32.self),
            contentVer: loadLE(base, 20, as: UInt32.self),
            frameBytes: loadLE(base, 24, as: UInt32.self),
            blockFirstShard: loadLE(base, 28, as: UInt16.self),
            shardIndex: loadLE(base, 30, as: UInt16.self),
            dataShards: p[32],
            parityShards: p[33],
            shardBytes: loadLE(base, 34, as: UInt16.self),
            totalDataShards: loadLE(base, 36, as: UInt16.self),
            deadlineBudgetMillis: loadLE(base, 38, as: UInt16.self),
            captureTimeMicros: loadLE(base, 40, as: UInt64.self))
        guard h.shardBytes > 0, h.dataShards > 0, h.totalDataShards > 0,
              Int(h.dataShards) + Int(h.parityShards) <= 255,
              Int(h.shardIndex) < Int(h.dataShards) + Int(h.parityShards),
              Int(h.blockFirstShard) + Int(h.dataShards) <= Int(h.totalDataShards),
              p.count - size >= Int(h.shardBytes),
              UInt64(h.frameBytes) <= UInt64(h.totalDataShards) * UInt64(h.shardBytes),
              UInt64(h.frameBytes) > UInt64(h.totalDataShards - 1) * UInt64(h.shardBytes)
        else { return nil }
        return h
    }

    public var isParity: Bool { shardIndex >= UInt16(dataShards) }
}

/// Cursor position, sent on every move plus a keepalive repeat (datagrams are unreliable; newest
/// `seq` wins). Shapes travel reliably as `CursorShape` on the control stream (plan §6.2).
public struct CursorDatagram: Sendable, Equatable {
    public static let size = 32

    public var visible: Bool
    public var monitorID: UInt16
    public var layoutGen: UInt32
    public var shapeVer: UInt32
    public var seq: UInt32
    public var x: Float     // hotspot position, monitor pixels
    public var y: Float
    public var hostTimeMicros: UInt64

    public init(visible: Bool, monitorID: UInt16, layoutGen: UInt32, shapeVer: UInt32, seq: UInt32, x: Float, y: Float, hostTimeMicros: UInt64) {
        self.visible = visible; self.monitorID = monitorID; self.layoutGen = layoutGen; self.shapeVer = shapeVer
        self.seq = seq; self.x = x; self.y = y; self.hostTimeMicros = hostTimeMicros
    }

    public func encoded() -> Data {
        var d = Data(count: Self.size)
        d.withUnsafeMutableBytes { raw in
            let p = raw.baseAddress!
            p.storeBytes(of: DatagramType.cursor.rawValue, as: UInt8.self)
            p.storeBytes(of: visible ? UInt8(1) : 0, toByteOffset: 1, as: UInt8.self)
            storeLE(p, 2, monitorID)
            storeLE(p, 4, layoutGen)
            storeLE(p, 8, shapeVer)
            storeLE(p, 12, seq)
            storeLE(p, 16, x.bitPattern)
            storeLE(p, 20, y.bitPattern)
            storeLE(p, 24, hostTimeMicros)
        }
        return d
    }

    public static func parse(_ p: UnsafeRawBufferPointer) -> CursorDatagram? {
        guard p.count >= size, let b = p.baseAddress, p[0] == DatagramType.cursor.rawValue else { return nil }
        let x = Float(bitPattern: loadLE(b, 16, as: UInt32.self))
        let y = Float(bitPattern: loadLE(b, 20, as: UInt32.self))
        guard x.isFinite, y.isFinite else { return nil }
        return CursorDatagram(visible: p[1] & 1 != 0, monitorID: loadLE(b, 2, as: UInt16.self),
                              layoutGen: loadLE(b, 4, as: UInt32.self), shapeVer: loadLE(b, 8, as: UInt32.self),
                              seq: loadLE(b, 12, as: UInt32.self), x: x, y: y, hostTimeMicros: loadLE(b, 24, as: UInt64.self))
    }
}

/// First datagram on the client's datagram flow. Network.framework surfaces a QUIC datagram flow on the
/// server as just another connection, so the flow announces itself explicitly.
public struct FlowHello: Sendable, Equatable {
    public static let size = 12
    public static let magic: UInt32 = 0x4744_5657  // "WVDG" little-endian

    public var sessionGen: UInt32

    public init(sessionGen: UInt32) { self.sessionGen = sessionGen }

    public func encoded() -> Data {
        var d = Data(count: Self.size)
        d.withUnsafeMutableBytes { raw in
            let p = raw.baseAddress!
            p.storeBytes(of: DatagramType.flowHello.rawValue, as: UInt8.self)
            p.storeBytes(of: UInt8(0), toByteOffset: 1, as: UInt8.self)
            storeLE(p, 2, UInt16(0))
            storeLE(p, 4, Self.magic)
            storeLE(p, 8, sessionGen)
        }
        return d
    }

    public static func parse(_ p: UnsafeRawBufferPointer) -> FlowHello? {
        guard p.count >= size, let b = p.baseAddress, p[0] == DatagramType.flowHello.rawValue,
              loadLE(b, 4, as: UInt32.self) == magic else { return nil }
        return FlowHello(sessionGen: loadLE(b, 8, as: UInt32.self))
    }
}

/// Retransmission request for data shards of one picture (global data-shard indices).
/// An empty index list means "the whole picture" (no shard of it ever arrived).
public struct NackDatagram: Sendable, Equatable {
    public static let headerSize = 16
    public static let maxIndices = 255

    public var monitorID: UInt16
    public var codecGen: UInt32
    public var picSeq: UInt32
    public var missing: [UInt16]

    public init(monitorID: UInt16, codecGen: UInt32, picSeq: UInt32, missing: [UInt16]) {
        self.monitorID = monitorID; self.codecGen = codecGen; self.picSeq = picSeq
        self.missing = Array(missing.prefix(Self.maxIndices))
    }

    public func encoded() -> Data {
        var d = Data(count: Self.headerSize + missing.count * 2)
        d.withUnsafeMutableBytes { raw in
            let p = raw.baseAddress!
            p.storeBytes(of: DatagramType.nack.rawValue, as: UInt8.self)
            p.storeBytes(of: UInt8(missing.count), toByteOffset: 1, as: UInt8.self)
            storeLE(p, 2, monitorID)
            storeLE(p, 4, codecGen)
            storeLE(p, 8, picSeq)
            storeLE(p, 12, UInt32(0))
            for (i, idx) in missing.enumerated() { storeLE(p, Self.headerSize + i * 2, idx) }
        }
        return d
    }

    public static func parse(_ p: UnsafeRawBufferPointer) -> NackDatagram? {
        guard p.count >= headerSize, let b = p.baseAddress, p[0] == DatagramType.nack.rawValue else { return nil }
        let count = Int(p[1])
        guard p.count >= headerSize + count * 2 else { return nil }
        var missing: [UInt16] = []
        missing.reserveCapacity(count)
        for i in 0..<count { missing.append(loadLE(b, headerSize + i * 2, as: UInt16.self)) }
        return NackDatagram(monitorID: loadLE(b, 2, as: UInt16.self), codecGen: loadLE(b, 4, as: UInt32.self),
                            picSeq: loadLE(b, 8, as: UInt32.self), missing: missing)
    }
}

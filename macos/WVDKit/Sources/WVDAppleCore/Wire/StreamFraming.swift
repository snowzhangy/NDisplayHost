import Foundation

/// Reliable-stream framing (core/proto/WIRE.md §3): every stream starts with the 4-byte preamble "WVD1",
/// then a sequence of frames `u32 little-endian length || Envelope protobuf`.
public enum StreamFraming {
    public static let preamble: [UInt8] = Array("WVD1".utf8)
    public static let maxFrameBytes = 4 << 20

    public static func frame(_ envelope: Envelope) -> Data {
        let body = envelope.serialized()
        var d = Data(capacity: 4 + body.count)
        withUnsafeBytes(of: UInt32(body.count).littleEndian) { d.append(contentsOf: $0) }
        d.append(contentsOf: body)
        return d
    }
}

public enum FramingError: Error, Equatable {
    case badPreamble
    case frameTooLarge(Int)
}

/// Incremental frame parser. Not thread-safe; owned by one connection queue.
public struct StreamDeframer {
    private var buffer: [UInt8] = []
    private var readIndex = 0
    private var sawPreamble: Bool

    public init(expectPreamble: Bool = true) { sawPreamble = !expectPreamble }

    public mutating func append(_ data: Data) throws -> [Envelope] {
        buffer.append(contentsOf: data)
        var out: [Envelope] = []
        if !sawPreamble {
            guard buffer.count - readIndex >= 4 else { return out }
            guard Array(buffer[readIndex..<readIndex + 4]) == StreamFraming.preamble else { throw FramingError.badPreamble }
            readIndex += 4
            sawPreamble = true
        }
        while buffer.count - readIndex >= 4 {
            let len = buffer.withUnsafeBytes { Int(loadLE($0.baseAddress!, readIndex, as: UInt32.self)) }
            guard len <= StreamFraming.maxFrameBytes else { throw FramingError.frameTooLarge(len) }
            guard buffer.count - readIndex - 4 >= len else { break }
            let start = readIndex + 4
            let env = try buffer.withUnsafeBytes { raw -> Envelope in
                var r = ProtoReader(UnsafeRawBufferPointer(rebasing: raw[start..<start + len]))
                return try Envelope(from: &r)
            }
            out.append(env)
            readIndex = start + len
        }
        // Compact once the consumed prefix dominates, keeping appends amortised O(1).
        if readIndex > 0 && readIndex >= buffer.count / 2 {
            buffer.removeFirst(readIndex)
            readIndex = 0
        }
        return out
    }
}

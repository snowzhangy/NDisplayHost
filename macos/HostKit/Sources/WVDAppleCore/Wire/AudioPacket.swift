import Foundation

/// Host -> client PCM audio (core/proto/WIRE.md §4 "Audio datagram", proto/WINDOWS_TCP.md "Audio").
///
/// QUIC carries it as a datagram (type 0x03). Little-endian, 28-byte header:
///
///     0  u8  type = 0x03          1  u8  format (1 = s16le interleaved)
///     2  u8  channels (1...8)     3  u8  reserved
///     4  u32 session_gen          8  u32 seq (per session, +1 per packet)
///     12 u32 sample_rate (Hz)     16 u64 host time of the first sample (µs, host clock)
///     24 u16 frames               26 u16 reserved
///     28 frames × channels × i16 samples
///
/// A packet carries at most `maxFrames` frames; hosts send 5 ms (240 frames at 48 kHz) so a stereo packet
/// stays below 1 KB and fits any QUIC datagram. Audio is uncompressed: 1.5 Mbit/s for 48 kHz stereo.
public struct AudioPacket: Sendable, Equatable {
    public static let headerSize = 28
    public static let maxFrames = 2048
    public static let formatS16 = UInt8(1)

    public var sessionGen: UInt32
    public var seq: UInt32
    public var sampleRate: UInt32
    public var channels: UInt8
    public var hostTimeMicros: UInt64
    /// Interleaved samples, `frames × channels`.
    public var samples: [Int16]

    public var frames: Int { channels > 0 ? samples.count / Int(channels) : 0 }

    public init(sessionGen: UInt32, seq: UInt32, sampleRate: UInt32, channels: UInt8, hostTimeMicros: UInt64, samples: [Int16]) {
        self.sessionGen = sessionGen; self.seq = seq; self.sampleRate = sampleRate; self.channels = channels
        self.hostTimeMicros = hostTimeMicros; self.samples = samples
    }

    public static func validFormat(sampleRate: UInt32, channels: Int, frames: Int) -> Bool {
        (8_000...192_000).contains(sampleRate) && (1...8).contains(channels) && (1...maxFrames).contains(frames)
    }

    public func encoded() -> Data {
        var d = Data(count: Self.headerSize + samples.count * 2)
        d.withUnsafeMutableBytes { raw in
            let p = raw.baseAddress!
            p.storeBytes(of: DatagramType.audio.rawValue, as: UInt8.self)
            p.storeBytes(of: Self.formatS16, toByteOffset: 1, as: UInt8.self)
            p.storeBytes(of: channels, toByteOffset: 2, as: UInt8.self)
            p.storeBytes(of: UInt8(0), toByteOffset: 3, as: UInt8.self)
            storeLE(p, 4, sessionGen)
            storeLE(p, 8, seq)
            storeLE(p, 12, sampleRate)
            storeLE(p, 16, hostTimeMicros)
            storeLE(p, 24, UInt16(clamping: frames))
            storeLE(p, 26, UInt16(0))
            for (i, s) in samples.enumerated() { storeLE(p, Self.headerSize + i * 2, s) }
        }
        return d
    }

    /// Parses and validates a datagram; nil for anything malformed (never trusts lengths from the wire).
    public static func parse(_ raw: UnsafeRawBufferPointer) -> AudioPacket? {
        guard raw.count >= headerSize, let b = raw.baseAddress, raw[0] == DatagramType.audio.rawValue,
              raw[1] == formatS16 else { return nil }
        let channels = Int(raw[2])
        let rate: UInt32 = loadLE(b, 12, as: UInt32.self)
        let frames = Int(loadLE(b, 24, as: UInt16.self))
        guard validFormat(sampleRate: rate, channels: channels, frames: frames),
              raw.count == headerSize + frames * channels * 2 else { return nil }
        let samples = (0..<frames * channels).map { loadLE(b, headerSize + $0 * 2, as: Int16.self) }
        return AudioPacket(sessionGen: loadLE(b, 4, as: UInt32.self), seq: loadLE(b, 8, as: UInt32.self), sampleRate: rate,
                           channels: UInt8(channels), hostTimeMicros: loadLE(b, 16, as: UInt64.self), samples: samples)
    }

    /// Splits interleaved PCM into packets of `framesPerPacket` (the last one may be shorter).
    public static func packetize(_ samples: [Int16], channels: Int, sampleRate: UInt32, framesPerPacket: Int,
                                 sessionGen: UInt32, firstSeq: UInt32, hostTimeMicros: UInt64) -> [AudioPacket] {
        guard validFormat(sampleRate: sampleRate, channels: channels, frames: framesPerPacket) else { return [] }
        let step = framesPerPacket * channels
        var out: [AudioPacket] = []
        var seq = firstSeq, offset = 0
        while offset < samples.count {
            let end = min(offset + step, samples.count - samples.count % channels)
            guard end > offset else { break }
            let t = hostTimeMicros &+ UInt64(offset / channels) * 1_000_000 / UInt64(max(sampleRate, 1))
            out.append(AudioPacket(sessionGen: sessionGen, seq: seq, sampleRate: sampleRate, channels: UInt8(channels),
                                   hostTimeMicros: t, samples: Array(samples[offset..<end])))
            seq &+= 1
            offset = end
        }
        return out
    }
}

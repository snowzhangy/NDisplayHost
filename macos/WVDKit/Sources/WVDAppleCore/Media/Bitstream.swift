import CoreMedia
import Foundation

/// NAL-unit helpers. Media payloads on the wire are length-prefixed NAL units (4-byte big-endian length,
/// "HVCC/AVCC style") with parameter sets carried out of band in `CodecConfig` (core/proto/WIRE.md §6).
/// Hosts whose encoders emit Annex B (NVENC, AMF) convert with `annexBToLengthPrefixed`.
public enum Bitstream {
    public enum BitstreamError: Error, Equatable {
        case malformed
        case missingParameterSets
        case formatDescription(OSStatus)
    }

    /// HEVC NAL unit type (ITU-T H.265 §7.4.2.2).
    @inline(__always) public static func hevcNALType(_ header: UInt8) -> UInt8 { (header >> 1) & 0x3F }

    /// H.264 NAL unit type.
    @inline(__always) public static func avcNALType(_ header: UInt8) -> UInt8 { header & 0x1F }

    /// Iterates length-prefixed NAL units. Returns false if the framing is inconsistent.
    @discardableResult
    public static func forEachNAL(_ au: UnsafeRawBufferPointer, lengthSize: Int = 4, _ body: (UnsafeRawBufferPointer) -> Void) -> Bool {
        var i = 0
        while i < au.count {
            guard au.count - i >= lengthSize else { return false }
            var len = 0
            for b in 0..<lengthSize { len = len << 8 | Int(au[i + b]) }
            i += lengthSize
            guard len > 0, len <= au.count - i else { return false }
            body(UnsafeRawBufferPointer(rebasing: au[i..<i + len]))
            i += len
        }
        return true
    }

    /// True if the access unit contains an IRAP picture (HEVC BLA/IDR/CRA, types 16-21) or an H.264 IDR.
    public static func isRandomAccess(_ au: UnsafeRawBufferPointer, codec: CodecKind, lengthSize: Int = 4) -> Bool {
        var found = false
        forEachNAL(au, lengthSize: lengthSize) { nal in
            guard let h = nal.first else { return }
            switch codec {
            case .hevc: if (16...21).contains(hevcNALType(h)) { found = true }
            case .h264: if avcNALType(h) == 5 { found = true }
            default: break
            }
        }
        return found
    }

    /// Converts Annex B (start-code delimited) to 4-byte length-prefixed NAL units, optionally
    /// extracting parameter sets (HEVC VPS/SPS/PPS = 32/33/34; H.264 SPS/PPS = 7/8) instead of keeping them in band.
    public static func annexBToLengthPrefixed(_ input: UnsafeRawBufferPointer, codec: CodecKind,
                                              extractParameterSets: Bool) -> (au: [UInt8], parameterSets: [[UInt8]]) {
        var nals: [Range<Int>] = []
        var i = 0
        var start: Int?
        while i + 2 < input.count {
            if input[i] == 0 && input[i + 1] == 0 && input[i + 2] == 1 {
                if let s = start {
                    var end = i
                    while end > s && input[end - 1] == 0 { end -= 1 }  // trailing zero / 4-byte start code
                    nals.append(s..<end)
                }
                i += 3
                start = i
            } else {
                i += 1
            }
        }
        if let s = start, s < input.count { nals.append(s..<input.count) }

        var au: [UInt8] = []
        au.reserveCapacity(input.count + nals.count * 4)
        var params: [[UInt8]] = []
        for r in nals where !r.isEmpty {
            let type = codec == .hevc ? hevcNALType(input[r.lowerBound]) : avcNALType(input[r.lowerBound])
            let isParam = codec == .hevc ? (32...34).contains(type) : (type == 7 || type == 8)
            if extractParameterSets && isParam {
                params.append(Array(input[r]))
                continue
            }
            let len = UInt32(r.count)
            au.append(contentsOf: [UInt8(len >> 24), UInt8(len >> 16 & 0xFF), UInt8(len >> 8 & 0xFF), UInt8(len & 0xFF)])
            au.append(contentsOf: input[r])
        }
        return (au, params)
    }

    /// Builds the CoreMedia format description for a `CodecConfig`.
    public static func formatDescription(for config: CodecConfig) throws -> CMVideoFormatDescription {
        guard !config.parameterSets.isEmpty else { throw BitstreamError.missingParameterSets }
        let sets = config.parameterSets
        // Pointers must stay valid for the call: copy into one contiguous allocation.
        let total = sets.reduce(0) { $0 + $1.count }
        let storage = UnsafeMutablePointer<UInt8>.allocate(capacity: max(total, 1))
        defer { storage.deallocate() }
        var pointers: [UnsafePointer<UInt8>] = []
        var sizes: [Int] = []
        var offset = 0
        for s in sets {
            s.withUnsafeBufferPointer { (storage + offset).update(from: $0.baseAddress!, count: s.count) }
            pointers.append(UnsafePointer(storage + offset))
            sizes.append(s.count)
            offset += s.count
        }
        var fd: CMFormatDescription?
        let nalLen = Int32(config.nalLengthSize == 0 ? 4 : config.nalLengthSize)
        let status: OSStatus
        switch config.codec {
        case .hevc:
            status = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                allocator: kCFAllocatorDefault, parameterSetCount: sets.count, parameterSetPointers: pointers,
                parameterSetSizes: sizes, nalUnitHeaderLength: nalLen, extensions: nil, formatDescriptionOut: &fd)
        case .h264:
            status = CMVideoFormatDescriptionCreateFromH264ParameterSets(
                allocator: kCFAllocatorDefault, parameterSetCount: sets.count, parameterSetPointers: pointers,
                parameterSetSizes: sizes, nalUnitHeaderLength: nalLen, formatDescriptionOut: &fd)
        default:
            throw BitstreamError.malformed
        }
        guard status == noErr, let fd else { throw BitstreamError.formatDescription(status) }
        return fd
    }
}

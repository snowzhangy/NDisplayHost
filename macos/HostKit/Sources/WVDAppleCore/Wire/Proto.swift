import Foundation

/// Minimal Protocol Buffers (proto3) wire-format codec.
///
/// The control stream is protobuf (plan §7) so the Windows host can use `protoc`-generated C++ from
/// `proto/wvd.proto`. The Apple side hand-encodes the same schema to stay dependency-free; the tests
/// cross-check encodings against `protoc --decode`. Hot-path datagrams are *not* protobuf (see
/// `Datagrams.swift`).
public enum ProtoError: Error, Equatable {
    case truncated
    case malformedVarint
    case unsupportedWireType(UInt8)
    case invalidUTF8
    case invalidValue(String)
}

public enum WireType: UInt8 {
    case varint = 0
    case fixed64 = 1
    case lengthDelimited = 2
    case fixed32 = 5
}

public struct ProtoWriter {
    public private(set) var bytes: [UInt8] = []

    public init(reserve: Int = 64) { bytes.reserveCapacity(reserve) }

    @inline(__always)
    public mutating func rawVarint(_ value: UInt64) {
        var v = value
        while v >= 0x80 {
            bytes.append(UInt8(truncatingIfNeeded: v) | 0x80)
            v >>= 7
        }
        bytes.append(UInt8(v))
    }

    @inline(__always)
    mutating func key(_ field: Int, _ type: WireType) {
        rawVarint(UInt64(field) << 3 | UInt64(type.rawValue))
    }

    // proto3 omits default values; decoders treat absent fields as zero/empty.
    public mutating func uint64(_ field: Int, _ value: UInt64) {
        guard value != 0 else { return }
        key(field, .varint); rawVarint(value)
    }

    public mutating func uint32(_ field: Int, _ value: UInt32) { uint64(field, UInt64(value)) }

    public mutating func enumValue<E: RawRepresentable>(_ field: Int, _ value: E) where E.RawValue == UInt32 {
        uint32(field, value.rawValue)
    }

    public mutating func sint64(_ field: Int, _ value: Int64) {
        guard value != 0 else { return }
        key(field, .varint); rawVarint(UInt64(bitPattern: (value << 1) ^ (value >> 63)))
    }

    public mutating func sint32(_ field: Int, _ value: Int32) { sint64(field, Int64(value)) }

    public mutating func bool(_ field: Int, _ value: Bool) {
        guard value else { return }
        key(field, .varint); bytes.append(1)
    }

    public mutating func float(_ field: Int, _ value: Float) {
        guard value != 0 || value.sign == .minus else { return }
        key(field, .fixed32)
        withUnsafeBytes(of: value.bitPattern.littleEndian) { bytes.append(contentsOf: $0) }
    }

    public mutating func bytesField(_ field: Int, _ value: [UInt8]) {
        guard !value.isEmpty else { return }
        key(field, .lengthDelimited); rawVarint(UInt64(value.count)); bytes.append(contentsOf: value)
    }

    public mutating func bytesField(_ field: Int, _ value: Data) {
        guard !value.isEmpty else { return }
        key(field, .lengthDelimited); rawVarint(UInt64(value.count)); bytes.append(contentsOf: value)
    }

    /// Repeated `bytes` keep empty elements (they are positional).
    public mutating func repeatedBytes(_ field: Int, _ values: [[UInt8]]) {
        for v in values {
            key(field, .lengthDelimited); rawVarint(UInt64(v.count)); bytes.append(contentsOf: v)
        }
    }

    public mutating func string(_ field: Int, _ value: String) {
        guard !value.isEmpty else { return }
        let utf8 = Array(value.utf8)
        key(field, .lengthDelimited); rawVarint(UInt64(utf8.count)); bytes.append(contentsOf: utf8)
    }

    /// Packed repeated uint32 (proto3 default for scalar repeated fields).
    public mutating func packedUInt32(_ field: Int, _ values: [UInt32]) {
        guard !values.isEmpty else { return }
        var inner = ProtoWriter()
        for v in values { inner.rawVarint(UInt64(v)) }
        key(field, .lengthDelimited); rawVarint(UInt64(inner.bytes.count)); bytes.append(contentsOf: inner.bytes)
    }

    /// Embedded message. Written even when empty when `always` is set (needed for `oneof` members).
    public mutating func message<M: ProtoMessage>(_ field: Int, _ value: M, always: Bool = false) {
        var inner = ProtoWriter()
        value.encode(to: &inner)
        guard always || !inner.bytes.isEmpty else { return }
        key(field, .lengthDelimited); rawVarint(UInt64(inner.bytes.count)); bytes.append(contentsOf: inner.bytes)
    }

    public mutating func optionalMessage<M: ProtoMessage>(_ field: Int, _ value: M?) {
        guard let value else { return }
        message(field, value, always: true)
    }

    public mutating func repeatedMessage<M: ProtoMessage>(_ field: Int, _ values: [M]) {
        for v in values { message(field, v, always: true) }
    }
}

public struct ProtoReader {
    private let bytes: UnsafeRawBufferPointer
    private var index: Int

    public init(_ bytes: UnsafeRawBufferPointer) {
        self.bytes = bytes
        self.index = 0
    }

    public var isAtEnd: Bool { index >= bytes.count }

    @inline(__always)
    mutating func rawVarint() throws -> UInt64 {
        var result: UInt64 = 0
        var shift: UInt64 = 0
        while true {
            guard index < bytes.count else { throw ProtoError.truncated }
            let b = bytes[index]
            index += 1
            if shift == 63 && b > 1 { throw ProtoError.malformedVarint }
            result |= UInt64(b & 0x7F) << shift
            if b < 0x80 { return result }
            shift += 7
            if shift > 63 { throw ProtoError.malformedVarint }
        }
    }

    /// Next field key, or `nil` at the end of the message.
    public mutating func nextField() throws -> (field: Int, type: WireType)? {
        guard !isAtEnd else { return nil }
        let key = try rawVarint()
        guard let type = WireType(rawValue: UInt8(key & 7)) else { throw ProtoError.unsupportedWireType(UInt8(key & 7)) }
        let field = Int(key >> 3)
        guard field > 0 else { throw ProtoError.invalidValue("field 0") }
        return (field, type)
    }

    public mutating func uint64() throws -> UInt64 { try rawVarint() }
    public mutating func uint32() throws -> UInt32 { UInt32(truncatingIfNeeded: try rawVarint()) }
    public mutating func bool() throws -> Bool { try rawVarint() != 0 }

    public mutating func sint64() throws -> Int64 {
        let v = try rawVarint()
        return Int64(bitPattern: (v >> 1) ^ (0 &- (v & 1)))
    }

    public mutating func sint32() throws -> Int32 { Int32(truncatingIfNeeded: try sint64()) }

    public mutating func float() throws -> Float {
        guard index + 4 <= bytes.count else { throw ProtoError.truncated }
        var raw: UInt32 = 0
        withUnsafeMutableBytes(of: &raw) { $0.copyMemory(from: UnsafeRawBufferPointer(rebasing: bytes[index..<index + 4])) }
        index += 4
        return Float(bitPattern: UInt32(littleEndian: raw))
    }

    public mutating func lengthDelimited() throws -> UnsafeRawBufferPointer {
        let len = try rawVarint()
        guard len <= UInt64(bytes.count - index) else { throw ProtoError.truncated }
        let slice = UnsafeRawBufferPointer(rebasing: bytes[index..<index + Int(len)])
        index += Int(len)
        return slice
    }

    public mutating func bytesValue() throws -> [UInt8] { Array(try lengthDelimited()) }

    public mutating func string() throws -> String {
        let raw = try lengthDelimited()
        guard let s = String(validating: raw.bindMemory(to: UInt8.self), as: UTF8.self) else { throw ProtoError.invalidUTF8 }
        return s
    }

    public mutating func message<M: ProtoMessage>(_: M.Type = M.self) throws -> M {
        let raw = try lengthDelimited()
        var sub = ProtoReader(raw)
        return try M(from: &sub)
    }

    /// Accepts both packed and unpacked encodings, as protobuf parsers must.
    public mutating func repeatedUInt32(type: WireType, into array: inout [UInt32]) throws {
        if type == .lengthDelimited {
            var sub = ProtoReader(try lengthDelimited())
            while !sub.isAtEnd { array.append(try sub.uint32()) }
        } else {
            array.append(try uint32())
        }
    }

    public mutating func skip(_ type: WireType) throws {
        switch type {
        case .varint: _ = try rawVarint()
        case .fixed64:
            guard index + 8 <= bytes.count else { throw ProtoError.truncated }
            index += 8
        case .fixed32:
            guard index + 4 <= bytes.count else { throw ProtoError.truncated }
            index += 4
        case .lengthDelimited: _ = try lengthDelimited()
        }
    }
}

public protocol ProtoMessage: Sendable, Equatable {
    init(from reader: inout ProtoReader) throws
    func encode(to writer: inout ProtoWriter)
}

extension ProtoMessage {
    public func serialized() -> [UInt8] {
        var w = ProtoWriter()
        encode(to: &w)
        return w.bytes
    }

    public init(serialized bytes: [UInt8]) throws {
        self = try bytes.withUnsafeBytes { raw in
            var r = ProtoReader(raw)
            return try Self(from: &r)
        }
    }

    public init(serialized data: Data) throws {
        self = try data.withUnsafeBytes { raw in
            var r = ProtoReader(raw)
            return try Self(from: &r)
        }
    }
}

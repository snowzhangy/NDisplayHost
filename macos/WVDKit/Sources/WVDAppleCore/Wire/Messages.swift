import Foundation

// Hand-written Swift codec for proto/wvd.proto (package wvd.v1). Field numbers must match the schema.
// Unknown fields are skipped, so newer peers can add fields without breaking older ones.

public let wvdProtocolVersion: UInt32 = 1

// MARK: - Enums

public enum ThermalLevel: UInt32, Sendable, CaseIterable {
    case nominal = 0, fair = 1, serious = 2, critical = 3
}

public enum AuthResult: UInt32, Sendable {
    case ok = 0, pairingRequired = 1, pairingStarted = 2, rejected = 3, lockedOut = 4, busy = 5
}

public enum InputArmState: UInt32, Sendable {
    case disarmed = 0, consentPending = 1, armed = 2, denied = 3
}

public enum CodecKind: UInt32, Sendable {
    case unknown = 0, hevc = 1, h264 = 2, av1 = 3
}

public enum CursorState: UInt32, Sendable {
    case separate = 0, embedded = 1, hidden = 2
}

public enum RecoveryKind: UInt32, Sendable {
    case idr = 0, invalidate = 1, ltr = 2
}

public enum MouseButton: UInt32, Sendable {
    case left = 0, right = 1, middle = 2
}

extension RawRepresentable where RawValue == UInt32 {
    static func decode(_ raw: UInt32, default value: Self) -> Self { Self(rawValue: raw) ?? value }
}

// MARK: - Messages

public struct DecoderCaps: ProtoMessage {
    public var hevc = false
    public var hevcMain10 = false
    public var h264 = false
    public var av1 = false
    public var maxWidth: UInt32 = 0
    public var maxHeight: UInt32 = 0
    public var maxPixelRate: UInt64 = 0

    public init(hevc: Bool = false, hevcMain10: Bool = false, h264: Bool = false, av1: Bool = false,
                maxWidth: UInt32 = 0, maxHeight: UInt32 = 0, maxPixelRate: UInt64 = 0) {
        self.hevc = hevc; self.hevcMain10 = hevcMain10; self.h264 = h264; self.av1 = av1
        self.maxWidth = maxWidth; self.maxHeight = maxHeight; self.maxPixelRate = maxPixelRate
    }

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: hevc = try r.bool()
            case 2: hevcMain10 = try r.bool()
            case 3: h264 = try r.bool()
            case 4: av1 = try r.bool()
            case 5: maxWidth = try r.uint32()
            case 6: maxHeight = try r.uint32()
            case 7: maxPixelRate = try r.uint64()
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) {
        w.bool(1, hevc); w.bool(2, hevcMain10); w.bool(3, h264); w.bool(4, av1)
        w.uint32(5, maxWidth); w.uint32(6, maxHeight); w.uint64(7, maxPixelRate)
    }
}

public struct Hello: ProtoMessage {
    public var protocolVersion: UInt32 = wvdProtocolVersion
    public var deviceID = ""
    public var deviceName = ""
    public var deviceModel = ""
    public var devicePublicKey: [UInt8] = []
    public var authSignature: [UInt8] = []
    public var requestPairing = false
    public var decoder: DecoderCaps?
    public var displayRefreshMilliHz: [UInt32] = []
    public var thermal: ThermalLevel = .nominal
    public var maxDatagramSize: UInt32 = 0

    public init() {}

    public init(from r: inout ProtoReader) throws {
        protocolVersion = 0
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: protocolVersion = try r.uint32()
            case 2: deviceID = try r.string()
            case 3: deviceName = try r.string()
            case 4: deviceModel = try r.string()
            case 5: devicePublicKey = try r.bytesValue()
            case 6: authSignature = try r.bytesValue()
            case 7: requestPairing = try r.bool()
            case 8: decoder = try r.message()
            case 9: try r.repeatedUInt32(type: t, into: &displayRefreshMilliHz)
            case 10: thermal = .decode(try r.uint32(), default: .nominal)
            case 11: maxDatagramSize = try r.uint32()
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) {
        w.uint32(1, protocolVersion); w.string(2, deviceID); w.string(3, deviceName); w.string(4, deviceModel)
        w.bytesField(5, devicePublicKey); w.bytesField(6, authSignature); w.bool(7, requestPairing)
        w.optionalMessage(8, decoder); w.packedUInt32(9, displayRefreshMilliHz); w.enumValue(10, thermal)
        w.uint32(11, maxDatagramSize)
    }
}

public struct HelloAck: ProtoMessage {
    public var protocolVersion: UInt32 = wvdProtocolVersion
    public var hostID = ""
    public var hostName = ""
    public var hostOS = ""
    public var auth: AuthResult = .ok
    public var sessionGen: UInt32 = 0
    public var hostTimeMicros: UInt64 = 0

    public init() {}

    public init(from r: inout ProtoReader) throws {
        protocolVersion = 0
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: protocolVersion = try r.uint32()
            case 2: hostID = try r.string()
            case 3: hostName = try r.string()
            case 4: hostOS = try r.string()
            case 5: auth = .decode(try r.uint32(), default: .rejected)
            case 6: sessionGen = try r.uint32()
            case 7: hostTimeMicros = try r.uint64()
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) {
        w.uint32(1, protocolVersion); w.string(2, hostID); w.string(3, hostName); w.string(4, hostOS)
        w.enumValue(5, auth); w.uint32(6, sessionGen); w.uint64(7, hostTimeMicros)
    }
}

public struct PairingChallenge: ProtoMessage {
    public var sasDigits: UInt32 = 6
    public var timeoutMillis: UInt32 = 0

    public init(sasDigits: UInt32 = 6, timeoutMillis: UInt32 = 0) {
        self.sasDigits = sasDigits; self.timeoutMillis = timeoutMillis
    }

    public init(from r: inout ProtoReader) throws {
        sasDigits = 0
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: sasDigits = try r.uint32()
            case 2: timeoutMillis = try r.uint32()
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) { w.uint32(1, sasDigits); w.uint32(2, timeoutMillis) }
}

public struct PairingConfirm: ProtoMessage {
    public var accepted = false
    public var deviceSignature: [UInt8] = []

    public init(accepted: Bool = false, deviceSignature: [UInt8] = []) {
        self.accepted = accepted; self.deviceSignature = deviceSignature
    }

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: accepted = try r.bool()
            case 2: deviceSignature = try r.bytesValue()
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) { w.bool(1, accepted); w.bytesField(2, deviceSignature) }
}

public struct PairingResult: ProtoMessage {
    public var success = false
    public var reason = ""
    public var hostCertSHA256: [UInt8] = []

    public init(success: Bool = false, reason: String = "", hostCertSHA256: [UInt8] = []) {
        self.success = success; self.reason = reason; self.hostCertSHA256 = hostCertSHA256
    }

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: success = try r.bool()
            case 2: reason = try r.string()
            case 3: hostCertSHA256 = try r.bytesValue()
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) { w.bool(1, success); w.string(2, reason); w.bytesField(3, hostCertSHA256) }
}

public struct SessionState: ProtoMessage {
    public var input: InputArmState = .disarmed
    public var inputGen: UInt32 = 0
    public var workstationLocked = false
    public var message = ""
    public var mediaFlowBound = false

    public init(input: InputArmState = .disarmed, inputGen: UInt32 = 0, workstationLocked: Bool = false,
                message: String = "", mediaFlowBound: Bool = false) {
        self.input = input; self.inputGen = inputGen; self.workstationLocked = workstationLocked
        self.message = message; self.mediaFlowBound = mediaFlowBound
    }

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: input = .decode(try r.uint32(), default: .disarmed)
            case 2: inputGen = try r.uint32()
            case 3: workstationLocked = try r.bool()
            case 4: message = try r.string()
            case 5: mediaFlowBound = try r.bool()
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) {
        w.enumValue(1, input); w.uint32(2, inputGen); w.bool(3, workstationLocked); w.string(4, message)
        w.bool(5, mediaFlowBound)
    }
}

public struct MonitorInfo: ProtoMessage, Identifiable, Hashable {
    public var monitorID: UInt32 = 0
    public var name = ""
    public var width: UInt32 = 0
    public var height: UInt32 = 0
    public var refreshMilliHz: UInt32 = 0
    public var ppi: UInt32 = 0
    public var desktopX: Int32 = 0
    public var desktopY: Int32 = 0
    public var primary = false

    public var id: UInt32 { monitorID }

    public init(monitorID: UInt32 = 0, name: String = "", width: UInt32 = 0, height: UInt32 = 0,
                refreshMilliHz: UInt32 = 0, ppi: UInt32 = 0, desktopX: Int32 = 0, desktopY: Int32 = 0, primary: Bool = false) {
        self.monitorID = monitorID; self.name = name; self.width = width; self.height = height
        self.refreshMilliHz = refreshMilliHz; self.ppi = ppi; self.desktopX = desktopX; self.desktopY = desktopY
        self.primary = primary
    }

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: monitorID = try r.uint32()
            case 2: name = try r.string()
            case 3: width = try r.uint32()
            case 4: height = try r.uint32()
            case 5: refreshMilliHz = try r.uint32()
            case 6: ppi = try r.uint32()
            case 7: desktopX = try r.sint32()
            case 8: desktopY = try r.sint32()
            case 9: primary = try r.bool()
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) {
        w.uint32(1, monitorID); w.string(2, name); w.uint32(3, width); w.uint32(4, height)
        w.uint32(5, refreshMilliHz); w.uint32(6, ppi); w.sint32(7, desktopX); w.sint32(8, desktopY); w.bool(9, primary)
    }
}

public struct MonitorLayout: ProtoMessage {
    public var layoutGen: UInt32 = 0
    public var monitors: [MonitorInfo] = []

    public init(layoutGen: UInt32 = 0, monitors: [MonitorInfo] = []) {
        self.layoutGen = layoutGen; self.monitors = monitors
    }

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: layoutGen = try r.uint32()
            case 2: monitors.append(try r.message())
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) { w.uint32(1, layoutGen); w.repeatedMessage(2, monitors) }
}

public struct ColorInfo: ProtoMessage {
    public var primaries: UInt32 = 0
    public var transfer: UInt32 = 0
    public var matrix: UInt32 = 0
    public var fullRange = false
    public var chromaLocation: UInt32 = 0

    /// Plan §6.7 SDR reference mode: BT.709 primaries, sRGB transfer, BT.709 matrix, full range, left siting.
    public static let sdrReference = ColorInfo(primaries: 1, transfer: 13, matrix: 1, fullRange: true, chromaLocation: 0)

    public init(primaries: UInt32 = 0, transfer: UInt32 = 0, matrix: UInt32 = 0, fullRange: Bool = false, chromaLocation: UInt32 = 0) {
        self.primaries = primaries; self.transfer = transfer; self.matrix = matrix
        self.fullRange = fullRange; self.chromaLocation = chromaLocation
    }

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: primaries = try r.uint32()
            case 2: transfer = try r.uint32()
            case 3: matrix = try r.uint32()
            case 4: fullRange = try r.bool()
            case 5: chromaLocation = try r.uint32()
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) {
        w.uint32(1, primaries); w.uint32(2, transfer); w.uint32(3, matrix); w.bool(4, fullRange); w.uint32(5, chromaLocation)
    }
}

public struct CodecConfig: ProtoMessage {
    public var monitorID: UInt32 = 0
    public var codecGen: UInt32 = 0
    public var codec: CodecKind = .unknown
    public var width: UInt32 = 0
    public var height: UInt32 = 0
    public var parameterSets: [[UInt8]] = []
    public var color: ColorInfo?
    public var nalLengthSize: UInt32 = 4
    public var frameRateMilliHz: UInt32 = 0

    public init() {}

    public init(from r: inout ProtoReader) throws {
        nalLengthSize = 0
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: monitorID = try r.uint32()
            case 2: codecGen = try r.uint32()
            case 3: codec = .decode(try r.uint32(), default: .unknown)
            case 4: width = try r.uint32()
            case 5: height = try r.uint32()
            case 6: parameterSets.append(try r.bytesValue())
            case 7: color = try r.message()
            case 8: nalLengthSize = try r.uint32()
            case 9: frameRateMilliHz = try r.uint32()
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) {
        w.uint32(1, monitorID); w.uint32(2, codecGen); w.enumValue(3, codec); w.uint32(4, width); w.uint32(5, height)
        w.repeatedBytes(6, parameterSets); w.optionalMessage(7, color); w.uint32(8, nalLengthSize); w.uint32(9, frameRateMilliHz)
    }
}

public struct CursorShape: ProtoMessage {
    public var monitorID: UInt32 = 0
    public var shapeVer: UInt32 = 0
    public var layoutGen: UInt32 = 0
    public var width: UInt32 = 0
    public var height: UInt32 = 0
    public var hotX: UInt32 = 0
    public var hotY: UInt32 = 0
    public var rgba: [UInt8] = []
    public var state: CursorState = .separate

    public init() {}

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: monitorID = try r.uint32()
            case 2: shapeVer = try r.uint32()
            case 3: layoutGen = try r.uint32()
            case 4: width = try r.uint32()
            case 5: height = try r.uint32()
            case 6: hotX = try r.uint32()
            case 7: hotY = try r.uint32()
            case 8: rgba = try r.bytesValue()
            case 9: state = .decode(try r.uint32(), default: .separate)
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) {
        w.uint32(1, monitorID); w.uint32(2, shapeVer); w.uint32(3, layoutGen); w.uint32(4, width); w.uint32(5, height)
        w.uint32(6, hotX); w.uint32(7, hotY); w.bytesField(8, rgba); w.enumValue(9, state)
    }

    public var isValid: Bool {
        width > 0 && height > 0 && width <= 512 && height <= 512 && rgba.count == Int(width * height * 4)
    }
}

public struct RecoveryRequest: ProtoMessage {
    public var monitorID: UInt32 = 0
    public var codecGen: UInt32 = 0
    public var kind: RecoveryKind = .idr
    public var lastGoodPicSeq: UInt32 = 0
    public var lostPicSeq: UInt32 = 0

    public init(monitorID: UInt32 = 0, codecGen: UInt32 = 0, kind: RecoveryKind = .idr, lastGoodPicSeq: UInt32 = 0, lostPicSeq: UInt32 = 0) {
        self.monitorID = monitorID; self.codecGen = codecGen; self.kind = kind
        self.lastGoodPicSeq = lastGoodPicSeq; self.lostPicSeq = lostPicSeq
    }

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: monitorID = try r.uint32()
            case 2: codecGen = try r.uint32()
            case 3: kind = .decode(try r.uint32(), default: .idr)
            case 4: lastGoodPicSeq = try r.uint32()
            case 5: lostPicSeq = try r.uint32()
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) {
        w.uint32(1, monitorID); w.uint32(2, codecGen); w.enumValue(3, kind); w.uint32(4, lastGoodPicSeq); w.uint32(5, lostPicSeq)
    }
}

public struct Ping: ProtoMessage {
    public var clientSendMicros: UInt64 = 0
    public var seq: UInt32 = 0

    public init(clientSendMicros: UInt64 = 0, seq: UInt32 = 0) { self.clientSendMicros = clientSendMicros; self.seq = seq }

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: clientSendMicros = try r.uint64()
            case 2: seq = try r.uint32()
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) { w.uint64(1, clientSendMicros); w.uint32(2, seq) }
}

public struct Pong: ProtoMessage {
    public var clientSendMicros: UInt64 = 0
    public var hostRecvMicros: UInt64 = 0
    public var hostSendMicros: UInt64 = 0
    public var seq: UInt32 = 0

    public init(clientSendMicros: UInt64 = 0, hostRecvMicros: UInt64 = 0, hostSendMicros: UInt64 = 0, seq: UInt32 = 0) {
        self.clientSendMicros = clientSendMicros; self.hostRecvMicros = hostRecvMicros
        self.hostSendMicros = hostSendMicros; self.seq = seq
    }

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: clientSendMicros = try r.uint64()
            case 2: hostRecvMicros = try r.uint64()
            case 3: hostSendMicros = try r.uint64()
            case 4: seq = try r.uint32()
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) {
        w.uint64(1, clientSendMicros); w.uint64(2, hostRecvMicros); w.uint64(3, hostSendMicros); w.uint32(4, seq)
    }
}

public struct QueueDepth: ProtoMessage {
    public var name = ""
    public var depth: UInt32 = 0
    public var capacity: UInt32 = 0
    public var peak: UInt32 = 0
    public var dropped: UInt64 = 0

    public init(name: String = "", depth: UInt32 = 0, capacity: UInt32 = 0, peak: UInt32 = 0, dropped: UInt64 = 0) {
        self.name = name; self.depth = depth; self.capacity = capacity; self.peak = peak; self.dropped = dropped
    }

    public init(name: String, _ s: QueueDepthSnapshot) {
        self.init(name: name, depth: UInt32(s.depth), capacity: UInt32(s.capacity), peak: UInt32(s.peak), dropped: s.dropped)
    }

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: name = try r.string()
            case 2: depth = try r.uint32()
            case 3: capacity = try r.uint32()
            case 4: peak = try r.uint32()
            case 5: dropped = try r.uint64()
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) {
        w.string(1, name); w.uint32(2, depth); w.uint32(3, capacity); w.uint32(4, peak); w.uint64(5, dropped)
    }
}

public struct ClientStats: ProtoMessage {
    public var monitorID: UInt32 = 0
    public var windowMicros: UInt64 = 0
    public var framesReceived: UInt32 = 0
    public var framesDecoded: UInt32 = 0
    public var framesPresented: UInt32 = 0
    public var framesLate: UInt32 = 0
    public var framesLost: UInt32 = 0
    public var shardsReceived: UInt32 = 0
    public var shardsMissing: UInt32 = 0
    public var shardsRecovered: UInt32 = 0
    public var bytesReceived: UInt64 = 0
    public var decodeMicrosP50: UInt32 = 0
    public var decodeMicrosP95: UInt32 = 0
    public var e2eMicrosP50: UInt32 = 0
    public var e2eMicrosP95: UInt32 = 0
    public var jitterMicros: UInt32 = 0
    public var queues: [QueueDepth] = []
    public var clockOffsetMicros: Int64 = 0
    public var rttMicros: UInt32 = 0
    public var nacksSent: UInt32 = 0
    public var framesSuperseded: UInt32 = 0
    public var framesPresentFailed: UInt32 = 0

    public init() {}

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: monitorID = try r.uint32()
            case 2: windowMicros = try r.uint64()
            case 3: framesReceived = try r.uint32()
            case 4: framesDecoded = try r.uint32()
            case 5: framesPresented = try r.uint32()
            case 6: framesLate = try r.uint32()
            case 7: framesLost = try r.uint32()
            case 8: shardsReceived = try r.uint32()
            case 9: shardsMissing = try r.uint32()
            case 10: shardsRecovered = try r.uint32()
            case 11: bytesReceived = try r.uint64()
            case 12: decodeMicrosP50 = try r.uint32()
            case 13: decodeMicrosP95 = try r.uint32()
            case 14: e2eMicrosP50 = try r.uint32()
            case 15: e2eMicrosP95 = try r.uint32()
            case 16: jitterMicros = try r.uint32()
            case 17: queues.append(try r.message())
            case 18: clockOffsetMicros = try r.sint64()
            case 19: rttMicros = try r.uint32()
            case 20: nacksSent = try r.uint32()
            case 21: framesSuperseded = try r.uint32()
            case 22: framesPresentFailed = try r.uint32()
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) {
        w.uint32(1, monitorID); w.uint64(2, windowMicros); w.uint32(3, framesReceived); w.uint32(4, framesDecoded)
        w.uint32(5, framesPresented); w.uint32(6, framesLate); w.uint32(7, framesLost); w.uint32(8, shardsReceived)
        w.uint32(9, shardsMissing); w.uint32(10, shardsRecovered); w.uint64(11, bytesReceived)
        w.uint32(12, decodeMicrosP50); w.uint32(13, decodeMicrosP95); w.uint32(14, e2eMicrosP50); w.uint32(15, e2eMicrosP95)
        w.uint32(16, jitterMicros); w.repeatedMessage(17, queues); w.sint64(18, clockOffsetMicros); w.uint32(19, rttMicros)
        w.uint32(20, nacksSent); w.uint32(21, framesSuperseded); w.uint32(22, framesPresentFailed)
    }
}

public struct HostStats: ProtoMessage {
    public var monitorID: UInt32 = 0
    public var fpsCaptured: UInt32 = 0
    public var fpsEncoded: UInt32 = 0
    public var bitrateKbps: UInt32 = 0
    public var encodeMicrosP50: UInt32 = 0
    public var encodeMicrosP95: UInt32 = 0
    public var queues: [QueueDepth] = []
    public var targetBitrateKbps: UInt32 = 0
    public var retransmittedShards: UInt32 = 0
    public var idrCount: UInt32 = 0

    public init() {}

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: monitorID = try r.uint32()
            case 2: fpsCaptured = try r.uint32()
            case 3: fpsEncoded = try r.uint32()
            case 4: bitrateKbps = try r.uint32()
            case 5: encodeMicrosP50 = try r.uint32()
            case 6: encodeMicrosP95 = try r.uint32()
            case 7: queues.append(try r.message())
            case 8: targetBitrateKbps = try r.uint32()
            case 9: retransmittedShards = try r.uint32()
            case 10: idrCount = try r.uint32()
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) {
        w.uint32(1, monitorID); w.uint32(2, fpsCaptured); w.uint32(3, fpsEncoded); w.uint32(4, bitrateKbps)
        w.uint32(5, encodeMicrosP50); w.uint32(6, encodeMicrosP95); w.repeatedMessage(7, queues)
        w.uint32(8, targetBitrateKbps); w.uint32(9, retransmittedShards); w.uint32(10, idrCount)
    }
}

public struct Visibility: ProtoMessage {
    public var visible = false
    public var monitorIDs: [UInt32] = []

    public init(visible: Bool = false, monitorIDs: [UInt32] = []) { self.visible = visible; self.monitorIDs = monitorIDs }

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: visible = try r.bool()
            case 2: try r.repeatedUInt32(type: t, into: &monitorIDs)
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) { w.bool(1, visible); w.packedUInt32(2, monitorIDs) }
}

public struct ThermalUpdate: ProtoMessage {
    public var state: ThermalLevel = .nominal

    public init(state: ThermalLevel = .nominal) { self.state = state }

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: state = .decode(try r.uint32(), default: .nominal)
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) { w.enumValue(1, state) }
}

public struct InputStreamOpen: ProtoMessage {
    public var sessionGen: UInt32 = 0

    public init(sessionGen: UInt32 = 0) { self.sessionGen = sessionGen }

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: sessionGen = try r.uint32()
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) { w.uint32(1, sessionGen) }
}

public struct PointerMove: ProtoMessage {
    public var monitorID: UInt32 = 0
    public var layoutGen: UInt32 = 0
    public var x: Float = 0
    public var y: Float = 0

    public init(monitorID: UInt32 = 0, layoutGen: UInt32 = 0, x: Float = 0, y: Float = 0) {
        self.monitorID = monitorID; self.layoutGen = layoutGen; self.x = x; self.y = y
    }

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: monitorID = try r.uint32()
            case 2: layoutGen = try r.uint32()
            case 3: x = try r.float()
            case 4: y = try r.float()
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) { w.uint32(1, monitorID); w.uint32(2, layoutGen); w.float(3, x); w.float(4, y) }
}

public struct PointerButton: ProtoMessage {
    public var monitorID: UInt32 = 0
    public var layoutGen: UInt32 = 0
    public var x: Float = 0
    public var y: Float = 0
    public var button: MouseButton = .left
    public var down = false
    public var clickCount: UInt32 = 0

    public init(monitorID: UInt32 = 0, layoutGen: UInt32 = 0, x: Float = 0, y: Float = 0, button: MouseButton = .left,
                down: Bool = false, clickCount: UInt32 = 1) {
        self.monitorID = monitorID; self.layoutGen = layoutGen; self.x = x; self.y = y
        self.button = button; self.down = down; self.clickCount = clickCount
    }

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: monitorID = try r.uint32()
            case 2: layoutGen = try r.uint32()
            case 3: x = try r.float()
            case 4: y = try r.float()
            case 5: button = .decode(try r.uint32(), default: .left)
            case 6: down = try r.bool()
            case 7: clickCount = try r.uint32()
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) {
        w.uint32(1, monitorID); w.uint32(2, layoutGen); w.float(3, x); w.float(4, y)
        w.enumValue(5, button); w.bool(6, down); w.uint32(7, clickCount)
    }
}

public struct ScrollEvent: ProtoMessage {
    public var dx: Float = 0
    public var dy: Float = 0
    public var precise = false

    public init(dx: Float = 0, dy: Float = 0, precise: Bool = true) { self.dx = dx; self.dy = dy; self.precise = precise }

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: dx = try r.float()
            case 2: dy = try r.float()
            case 3: precise = try r.bool()
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) { w.float(1, dx); w.float(2, dy); w.bool(3, precise) }
}

public struct KeyEvent: ProtoMessage {
    public var hidUsage: UInt32 = 0
    public var down = false

    public init(hidUsage: UInt32 = 0, down: Bool = false) { self.hidUsage = hidUsage; self.down = down }

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: hidUsage = try r.uint32()
            case 2: down = try r.bool()
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) { w.uint32(1, hidUsage); w.bool(2, down) }
}

public struct TextEvent: ProtoMessage {
    public var text = ""

    public init(text: String = "") { self.text = text }

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: text = try r.string()
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) { w.string(1, text) }
}

public struct InputEvent: ProtoMessage {
    public enum Kind: Sendable, Equatable {
        case move(PointerMove)
        case button(PointerButton)
        case scroll(ScrollEvent)
        case key(KeyEvent)
        case text(TextEvent)
    }

    public var seq: UInt32 = 0
    public var clientTimeMicros: UInt64 = 0
    public var kind: Kind?

    public init(seq: UInt32 = 0, clientTimeMicros: UInt64 = 0, kind: Kind? = nil) {
        self.seq = seq; self.clientTimeMicros = clientTimeMicros; self.kind = kind
    }

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: seq = try r.uint32()
            case 2: clientTimeMicros = try r.uint64()
            case 3: kind = .move(try r.message())
            case 4: kind = .button(try r.message())
            case 5: kind = .scroll(try r.message())
            case 6: kind = .key(try r.message())
            case 7: kind = .text(try r.message())
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) {
        w.uint32(1, seq); w.uint64(2, clientTimeMicros)
        switch kind {
        case .move(let m): w.message(3, m, always: true)
        case .button(let m): w.message(4, m, always: true)
        case .scroll(let m): w.message(5, m, always: true)
        case .key(let m): w.message(6, m, always: true)
        case .text(let m): w.message(7, m, always: true)
        case nil: break
        }
    }
}

public struct InputEvents: ProtoMessage {
    public var inputGen: UInt32 = 0
    public var events: [InputEvent] = []

    public init(inputGen: UInt32 = 0, events: [InputEvent] = []) { self.inputGen = inputGen; self.events = events }

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: inputGen = try r.uint32()
            case 2: events.append(try r.message())
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) { w.uint32(1, inputGen); w.repeatedMessage(2, events) }
}

public struct InputRelease: ProtoMessage {
    public var inputGen: UInt32 = 0

    public init(inputGen: UInt32 = 0) { self.inputGen = inputGen }

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: inputGen = try r.uint32()
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) { w.uint32(1, inputGen) }
}

public struct StreamSettings: ProtoMessage {
    public var maxBitrateKbps: UInt32 = 0
    public var frameRate: UInt32 = 0
    public var preferQuality = false
    /// The client wants the host's audio (audio datagrams). Off by default: hosts send none.
    public var audio = false

    public init(maxBitrateKbps: UInt32 = 0, frameRate: UInt32 = 0, preferQuality: Bool = false, audio: Bool = false) {
        self.maxBitrateKbps = maxBitrateKbps; self.frameRate = frameRate; self.preferQuality = preferQuality; self.audio = audio
    }

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: maxBitrateKbps = try r.uint32()
            case 2: frameRate = try r.uint32()
            case 3: preferQuality = try r.bool()
            case 4: audio = try r.bool()
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) {
        w.uint32(1, maxBitrateKbps); w.uint32(2, frameRate); w.bool(3, preferQuality); w.bool(4, audio)
    }
}

public struct Goodbye: ProtoMessage {
    public var reason = ""

    public init(reason: String = "") { self.reason = reason }

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: reason = try r.string()
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) { w.string(1, reason) }
}

// MARK: - Display size (the monitor window's size menu)

/// Outcome of a `DisplayModeRequest`, echoed in the `DisplayModes` that answers it.
public enum ModeChangeStatus: UInt32, Sendable {
    /// A list nobody asked for: session start, or the size changed.
    case unsolicited = 0, applied = 1, unchanged = 2, failed = 3, unsupported = 4
}

/// One size the host's monitor can run at.
public struct DisplayMode: ProtoMessage, Hashable {
    public var width: UInt32 = 0
    public var height: UInt32 = 0
    public var refreshMilliHz: UInt32 = 0
    public var current = false
    public var native = false
    /// Larger than the host's encode budget: offered, but scaled down before encoding.
    public var scaledForEncode = false

    public init(width: UInt32 = 0, height: UInt32 = 0, refreshMilliHz: UInt32 = 0, current: Bool = false,
                native: Bool = false, scaledForEncode: Bool = false) {
        self.width = width; self.height = height; self.refreshMilliHz = refreshMilliHz
        self.current = current; self.native = native; self.scaledForEncode = scaledForEncode
    }

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: width = try r.uint32()
            case 2: height = try r.uint32()
            case 3: refreshMilliHz = try r.uint32()
            case 4: current = try r.bool()
            case 5: native = try r.bool()
            case 6: scaledForEncode = try r.bool()
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) {
        w.uint32(1, width); w.uint32(2, height); w.uint32(3, refreshMilliHz)
        w.bool(4, current); w.bool(5, native); w.bool(6, scaledForEncode)
    }
}

/// Host -> client: the sizes a monitor offers. Sent at session start, after every size change, and as
/// the answer to a `DisplayModeRequest` (`requestID` != 0).
public struct DisplayModes: ProtoMessage {
    public var monitorID: UInt32 = 0
    public var layoutGen: UInt32 = 0
    public var modes: [DisplayMode] = []
    public var requestID: UInt32 = 0
    public var status: ModeChangeStatus = .unsolicited

    public init(monitorID: UInt32 = 0, layoutGen: UInt32 = 0, modes: [DisplayMode] = [], requestID: UInt32 = 0,
                status: ModeChangeStatus = .unsolicited) {
        self.monitorID = monitorID; self.layoutGen = layoutGen; self.modes = modes
        self.requestID = requestID; self.status = status
    }

    public var current: DisplayMode? { modes.first(where: \.current) }

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: monitorID = try r.uint32()
            case 2: layoutGen = try r.uint32()
            case 3: modes.append(try r.message())
            case 4: requestID = try r.uint32()
            case 5: status = .decode(try r.uint32(), default: .unsolicited)
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) {
        w.uint32(1, monitorID); w.uint32(2, layoutGen); w.repeatedMessage(3, modes)
        w.uint32(4, requestID); w.enumValue(5, status)
    }
}

/// Client -> host: run this monitor at one of its listed sizes for the rest of the session.
public struct DisplayModeRequest: ProtoMessage {
    public var monitorID: UInt32 = 0
    public var width: UInt32 = 0
    public var height: UInt32 = 0
    /// 0 = the host chooses (it keeps the current rate when the size offers it).
    public var refreshMilliHz: UInt32 = 0
    public var requestID: UInt32 = 0

    public init(monitorID: UInt32 = 0, width: UInt32 = 0, height: UInt32 = 0, refreshMilliHz: UInt32 = 0,
                requestID: UInt32 = 0) {
        self.monitorID = monitorID; self.width = width; self.height = height
        self.refreshMilliHz = refreshMilliHz; self.requestID = requestID
    }

    public init(from r: inout ProtoReader) throws {
        while let (f, t) = try r.nextField() {
            switch f {
            case 1: monitorID = try r.uint32()
            case 2: width = try r.uint32()
            case 3: height = try r.uint32()
            case 4: refreshMilliHz = try r.uint32()
            case 5: requestID = try r.uint32()
            default: try r.skip(t)
            }
        }
    }

    public func encode(to w: inout ProtoWriter) {
        w.uint32(1, monitorID); w.uint32(2, width); w.uint32(3, height); w.uint32(4, refreshMilliHz)
        w.uint32(5, requestID)
    }
}

// MARK: - Envelope

public enum Envelope: ProtoMessage {
    case hello(Hello)
    case helloAck(HelloAck)
    case pairingChallenge(PairingChallenge)
    case pairingConfirm(PairingConfirm)
    case pairingResult(PairingResult)
    case sessionState(SessionState)
    case monitorLayout(MonitorLayout)
    case codecConfig(CodecConfig)
    case cursorShape(CursorShape)
    case recoveryRequest(RecoveryRequest)
    case ping(Ping)
    case pong(Pong)
    case clientStats(ClientStats)
    case hostStats(HostStats)
    case visibility(Visibility)
    case thermal(ThermalUpdate)
    case input(InputEvents)
    case inputRelease(InputRelease)
    case streamSettings(StreamSettings)
    case goodbye(Goodbye)
    case inputStreamOpen(InputStreamOpen)
    case displayModes(DisplayModes)
    case displayModeRequest(DisplayModeRequest)
    /// A message type this build does not know (newer peer). Ignored by receivers.
    case unknown(field: Int)

    public init(from r: inout ProtoReader) throws {
        var result: Envelope?
        while let (f, t) = try r.nextField() {
            guard t == .lengthDelimited else { try r.skip(t); continue }
            switch f {
            case 1: result = .hello(try r.message())
            case 2: result = .helloAck(try r.message())
            case 3: result = .pairingChallenge(try r.message())
            case 4: result = .pairingConfirm(try r.message())
            case 5: result = .pairingResult(try r.message())
            case 6: result = .sessionState(try r.message())
            case 7: result = .monitorLayout(try r.message())
            case 8: result = .codecConfig(try r.message())
            case 9: result = .cursorShape(try r.message())
            case 10: result = .recoveryRequest(try r.message())
            case 11: result = .ping(try r.message())
            case 12: result = .pong(try r.message())
            case 13: result = .clientStats(try r.message())
            case 14: result = .hostStats(try r.message())
            case 15: result = .visibility(try r.message())
            case 16: result = .thermal(try r.message())
            case 17: result = .input(try r.message())
            case 18: result = .inputRelease(try r.message())
            case 19: result = .streamSettings(try r.message())
            case 20: result = .goodbye(try r.message())
            case 21: result = .inputStreamOpen(try r.message())
            case 22: result = .displayModes(try r.message())
            case 23: result = .displayModeRequest(try r.message())
            default:
                try r.skip(t)
                result = .unknown(field: f)
            }
        }
        guard let result else { throw ProtoError.invalidValue("empty envelope") }
        self = result
    }

    public func encode(to w: inout ProtoWriter) {
        switch self {
        case .hello(let m): w.message(1, m, always: true)
        case .helloAck(let m): w.message(2, m, always: true)
        case .pairingChallenge(let m): w.message(3, m, always: true)
        case .pairingConfirm(let m): w.message(4, m, always: true)
        case .pairingResult(let m): w.message(5, m, always: true)
        case .sessionState(let m): w.message(6, m, always: true)
        case .monitorLayout(let m): w.message(7, m, always: true)
        case .codecConfig(let m): w.message(8, m, always: true)
        case .cursorShape(let m): w.message(9, m, always: true)
        case .recoveryRequest(let m): w.message(10, m, always: true)
        case .ping(let m): w.message(11, m, always: true)
        case .pong(let m): w.message(12, m, always: true)
        case .clientStats(let m): w.message(13, m, always: true)
        case .hostStats(let m): w.message(14, m, always: true)
        case .visibility(let m): w.message(15, m, always: true)
        case .thermal(let m): w.message(16, m, always: true)
        case .input(let m): w.message(17, m, always: true)
        case .inputRelease(let m): w.message(18, m, always: true)
        case .streamSettings(let m): w.message(19, m, always: true)
        case .goodbye(let m): w.message(20, m, always: true)
        case .inputStreamOpen(let m): w.message(21, m, always: true)
        case .displayModes(let m): w.message(22, m, always: true)
        case .displayModeRequest(let m): w.message(23, m, always: true)
        case .unknown: break
        }
    }
}

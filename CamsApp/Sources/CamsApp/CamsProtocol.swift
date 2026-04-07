// CamsProtocol.swift
// Cams — Custom UDP protocol definitions shared between the iOS sender and OBS receiver.
//
// Packet wire format (big-endian):
//  ┌────────────────────┬────────────────────┬────────────────────┬────────────────────┐
//  │  Sequence Number   │     Timestamp      │   Frame Type (1)   │  Payload Length    │
//  │     4 bytes        │     8 bytes        │                    │     4 bytes        │
//  ├────────────────────┴────────────────────┴────────────────────┴────────────────────┤
//  │                          Payload (variable length)                                │
//  └───────────────────────────────────────────────────────────────────────────────────┘
// Total header size: 17 bytes.
//
// Control back-channel (port 8889) uses a separate lightweight command structure.

import Foundation

// MARK: - Constants

/// The Bonjour service type used for zero-config device discovery.
public let kCamsServiceType = "_cams-video._udp"
/// Primary UDP port for video streaming.
public let kCamsVideoPort: UInt16 = 8888
/// UDP port for the bidirectional control back-channel.
public let kCamsControlPort: UInt16 = 8889
/// Wire-format header size in bytes.
public let kCamsHeaderSize: Int = 17

// MARK: - Frame Types

/// Identifies the content carried in a packet payload.
public enum FrameType: UInt8, Sendable {
    /// H.264 / AVC NAL unit(s).
    case h264      = 0x01
    /// HEVC / H.265 NAL unit(s).
    case hevc      = 0x02
    /// Encoder parameter sets (SPS/PPS/VPS) sent before the first IDR frame.
    case parameterSets = 0x03
    /// Keyframe (IDR) marker — same codec as the current session.
    case keyframe  = 0x04
    /// End-of-stream signal.
    case eos       = 0xFF

    /// Returns `true` when this frame type carries decodable video data.
    public var isVideoData: Bool {
        switch self {
        case .h264, .hevc, .keyframe, .parameterSets: return true
        case .eos: return false
        }
    }
}

// MARK: - Control Commands

/// Commands sent over the UDP control back-channel from OBS → iOS.
public enum ControlCommand: UInt8, Sendable {
    /// Lock auto-focus at the current position.
    case lockFocus     = 0x01
    /// Unlock auto-focus and return to continuous AF.
    case unlockFocus   = 0x02
    /// Lock auto-exposure at the current settings.
    case lockExposure  = 0x03
    /// Unlock auto-exposure.
    case unlockExposure = 0x04
    /// Request an immediate keyframe (IDR) from the encoder.
    case requestKeyframe = 0x05
    /// Ping — used to measure round-trip latency.
    case ping          = 0xFE
    /// Pong — response to a ping.
    case pong          = 0xFF
}

// MARK: - CamsPacketHeader

/// A strongly-typed, zero-copy view over a serialised Cams packet header.
public struct CamsPacketHeader: Sendable {
    // Raw field storage — all values are in host byte order after parsing.
    public let sequenceNumber: UInt32
    public let timestamp: UInt64
    public let frameType: FrameType
    public let payloadLength: UInt32

    // MARK: Initialisation

    /// Constructs a header from individual fields.
    public init(
        sequenceNumber: UInt32,
        timestamp: UInt64,
        frameType: FrameType,
        payloadLength: UInt32
    ) {
        self.sequenceNumber = sequenceNumber
        self.timestamp      = timestamp
        self.frameType      = frameType
        self.payloadLength  = payloadLength
    }

    // MARK: Serialisation

    /// Encodes the header into exactly `kCamsHeaderSize` (17) bytes in big-endian order.
    public func serialise() -> Data {
        var data = Data(capacity: kCamsHeaderSize)
        // Sequence number — 4 bytes, big-endian.
        var seq = sequenceNumber.bigEndian
        withUnsafeBytes(of: &seq) { data.append(contentsOf: $0) }
        // Timestamp — 8 bytes, big-endian.
        var ts = timestamp.bigEndian
        withUnsafeBytes(of: &ts) { data.append(contentsOf: $0) }
        // Frame type — 1 byte.
        data.append(frameType.rawValue)
        // Payload length — 4 bytes, big-endian.
        var len = payloadLength.bigEndian
        withUnsafeBytes(of: &len) { data.append(contentsOf: $0) }
        return data
    }

    // MARK: Deserialisation

    /// Parses a header from the first `kCamsHeaderSize` bytes of `data`.
    ///
    /// Returns `nil` if `data` is too short or the frame-type byte is unrecognised.
    public static func deserialise(from data: Data) -> CamsPacketHeader? {
        guard data.count >= kCamsHeaderSize else { return nil }

        let bytes = data.withUnsafeBytes { $0 }

        let seqBE   = bytes.load(fromByteOffset: 0,  as: UInt32.self)
        let tsBE    = bytes.load(fromByteOffset: 4,  as: UInt64.self)
        let ftByte  = bytes.load(fromByteOffset: 12, as: UInt8.self)
        let lenBE   = bytes.load(fromByteOffset: 13, as: UInt32.self)

        guard let frameType = FrameType(rawValue: ftByte) else { return nil }

        return CamsPacketHeader(
            sequenceNumber: UInt32(bigEndian: seqBE),
            timestamp:      UInt64(bigEndian: tsBE),
            frameType:      frameType,
            payloadLength:  UInt32(bigEndian: lenBE)
        )
    }
}

// MARK: - CamsControlPacket

/// A compact control packet sent over the back-channel.
///
/// Wire format: [Command (1 byte) | Payload (variable, 0–255 bytes)]
public struct CamsControlPacket: Sendable {
    public let command: ControlCommand
    public let payload: Data

    public init(command: ControlCommand, payload: Data = Data()) {
        self.command = command
        self.payload = payload
    }

    /// Serialises the control packet to bytes.
    public func serialise() -> Data {
        var data = Data(capacity: 1 + payload.count)
        data.append(command.rawValue)
        data.append(payload)
        return data
    }

    /// Parses a control packet from raw bytes.
    public static func deserialise(from data: Data) -> CamsControlPacket? {
        guard let first = data.first,
              let command = ControlCommand(rawValue: first) else { return nil }
        let payload = data.count > 1 ? data.dropFirst() : Data()
        return CamsControlPacket(command: command, payload: payload)
    }
}

// MARK: - Timestamp Helpers

/// Returns the current monotonic timestamp in nanoseconds, suitable for packet headers.
@inline(__always)
public func camsCurrentTimestampNs() -> UInt64 {
    var info = mach_timebase_info_data_t()
    mach_timebase_info(&info)
    let ticks = mach_absolute_time()
    return ticks * UInt64(info.numer) / UInt64(info.denom)
}

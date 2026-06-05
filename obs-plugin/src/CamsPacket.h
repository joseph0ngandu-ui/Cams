// CamsPacket.h
// Cams OBS Plugin — Shared packet definitions, mirroring CamsProtocol.swift.
//
// Wire format (big-endian):
//  [Sequence Number (4)] [Timestamp (8)] [Frame Type (1)]
//  [Fragment Index (2)] [Fragment Count (2)] [Payload Length (4)] [Payload]
//
// All multi-byte fields are transmitted in network (big-endian) byte order.

#pragma once

// ---------------------------------------------------------------------------
// Platform byte-order helpers — must be included before the struct definitions
// that use htobe32 / be32toh / htobe64 / be64toh.
// ---------------------------------------------------------------------------
#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "Ws2_32.lib")
static inline uint64_t _cams_htobe64(uint64_t v) {
    return (static_cast<uint64_t>(htonl(static_cast<uint32_t>(v & 0xFFFFFFFFULL))) << 32)
         | htonl(static_cast<uint32_t>(v >> 32));
}
#define htobe16(x)  htons(x)
#define be16toh(x)  ntohs(x)
#define htobe32(x)  htonl(x)
#define be32toh(x)  ntohl(x)
#define htobe64(x)  _cams_htobe64(x)
#define be64toh(x)  _cams_htobe64(x)
#else
// Linux and macOS both provide <endian.h> or <sys/endian.h>.
// On macOS this pulls in machine/endian.h (libkern/OSByteOrder.h) which defines
// OSSwapInt32 etc.  The POSIX names htobe32/be32toh come from <arpa/inet.h> on
// macOS and from <endian.h> on Linux.
#ifdef __APPLE__
#include <machine/endian.h>
#include <libkern/OSByteOrder.h>
#ifndef htobe32
#define htobe16(x)  OSSwapHostToBigInt16(x)
#define be16toh(x)  OSSwapBigToHostInt16(x)
#define htobe32(x)  OSSwapHostToBigInt32(x)
#define be32toh(x)  OSSwapBigToHostInt32(x)
#define htobe64(x)  OSSwapHostToBigInt64(x)
#define be64toh(x)  OSSwapBigToHostInt64(x)
#endif
#else
// Linux
#include <endian.h>
#endif
#endif

#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace cams {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Bonjour service type used for device discovery.
static constexpr const char *kServiceType = "_cams-video._udp";
/// Primary video streaming port.
static constexpr uint16_t kVideoPort   = 8888;
/// Control back-channel port.
static constexpr uint16_t kControlPort = 8889;
/// Fixed header size in bytes.
static constexpr size_t   kHeaderSize  = 21;
/// Largest UDP payload accepted by the receiver after the Cams header.
static constexpr size_t   kMaxDatagramPayloadSize = 65536 - kHeaderSize;
/// Defensive cap on fragments per encoded frame.
static constexpr uint16_t kMaxFragments = 1024;

// ---------------------------------------------------------------------------
// Frame Types
// ---------------------------------------------------------------------------

enum class FrameType : uint8_t {
    H264          = 0x01,
    HEVC          = 0x02,
    ParameterSets = 0x03,
    Keyframe      = 0x04,
    AudioPCM      = 0x10, ///< Reserved for future audio transport.
    EndOfStream   = 0xFF,
};

/// Returns true for frame types that carry decodable video.
inline bool isVideoData(FrameType ft) {
    switch (ft) {
    case FrameType::H264:
    case FrameType::HEVC:
    case FrameType::ParameterSets:
    case FrameType::Keyframe:
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Control Commands
// ---------------------------------------------------------------------------

enum class ControlCommand : uint8_t {
    LockFocus        = 0x01,
    UnlockFocus      = 0x02,
    LockExposure     = 0x03,
    UnlockExposure   = 0x04,
    RequestKeyframe  = 0x05,
    SetQualityLow    = 0x06,
    SetQualityMedium = 0x07,
    SetQualityHigh   = 0x08,
    SetAudioEnabled  = 0x09, ///< Reserved for future audio transport.
    SetCenterStageEnabled = 0x0A, ///< Enable/disable subject tracking. Payload: 0x00=off, 0x01=on.
    Ping             = 0xFE,
    Pong             = 0xFF,
};

// ---------------------------------------------------------------------------
// PacketHeader
// ---------------------------------------------------------------------------

/// Strongly-typed representation of a Cams packet header.
struct PacketHeader {
    uint32_t  sequenceNumber;  ///< Monotonically increasing encoded-frame counter.
    uint64_t  timestamp;       ///< Capture timestamp in nanoseconds.
    FrameType frameType;       ///< Content type of the enclosed payload.
    uint16_t  fragmentIndex;   ///< Fragment index (0-based).
    uint16_t  fragmentCount;   ///< Total number of fragments for this frame.
    uint32_t  payloadLength;   ///< Byte length of the payload that follows.

    // -----------------------------------------------------------------------
    // Serialisation / Deserialisation

    /// Serialises the header into exactly kHeaderSize bytes (big-endian).
    void serialise(uint8_t out[kHeaderSize]) const {
        uint32_t seqBE   = htobe32(sequenceNumber);
        uint64_t tsBE    = htobe64(timestamp);
        uint16_t fIdxBE  = htobe16(fragmentIndex);
        uint16_t fCntBE  = htobe16(fragmentCount);
        uint32_t lenBE   = htobe32(payloadLength);

        size_t offset = 0;
        std::memcpy(out + offset, &seqBE,  4);  offset += 4;
        std::memcpy(out + offset, &tsBE,   8);  offset += 8;
        out[offset] = static_cast<uint8_t>(frameType);  ++offset;
        std::memcpy(out + offset, &fIdxBE, 2);  offset += 2;
        std::memcpy(out + offset, &fCntBE, 2);  offset += 2;
        std::memcpy(out + offset, &lenBE,  4);
    }

    /// Parses a PacketHeader from `data` (must be at least kHeaderSize bytes).
    /// Returns std::nullopt if data is too short or the frame type is invalid.
    static std::optional<PacketHeader> deserialise(
        const uint8_t *data,
        size_t length
    ) {
        if (length < kHeaderSize) return std::nullopt;

        PacketHeader h;
        uint32_t seqBE;
        uint64_t tsBE;
        uint16_t fIdxBE;
        uint16_t fCntBE;
        uint32_t lenBE;

        std::memcpy(&seqBE,  data,      4);
        std::memcpy(&tsBE,   data + 4,  8);
        uint8_t ftByte = data[12];
        std::memcpy(&fIdxBE, data + 13, 2);
        std::memcpy(&fCntBE, data + 15, 2);
        std::memcpy(&lenBE,  data + 17, 4);

        // Validate frame type.
        switch (static_cast<FrameType>(ftByte)) {
        case FrameType::H264:
        case FrameType::HEVC:
        case FrameType::ParameterSets:
        case FrameType::Keyframe:
        case FrameType::AudioPCM:
        case FrameType::EndOfStream:
            break;
        default:
            return std::nullopt;
        }

        h.sequenceNumber = be32toh(seqBE);
        h.timestamp      = be64toh(tsBE);
        h.frameType      = static_cast<FrameType>(ftByte);
        h.fragmentIndex  = be16toh(fIdxBE);
        h.fragmentCount  = be16toh(fCntBE);
        h.payloadLength  = be32toh(lenBE);
        if (h.fragmentCount == 0 ||
            h.fragmentIndex >= h.fragmentCount ||
            h.fragmentCount > kMaxFragments ||
            h.payloadLength > kMaxDatagramPayloadSize)
        {
            return std::nullopt;
        }
        return h;
    }
};

// ---------------------------------------------------------------------------
// ControlPacket
// ---------------------------------------------------------------------------

struct ControlPacket {
    ControlCommand       command;
    std::vector<uint8_t> payload;

    std::vector<uint8_t> serialise() const {
        std::vector<uint8_t> out;
        out.reserve(1 + payload.size());
        out.push_back(static_cast<uint8_t>(command));
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    }

    static std::optional<ControlPacket> deserialise(
        const uint8_t *data,
        size_t length
    ) {
        if (length == 0) return std::nullopt;
        ControlPacket pkt;
        switch (static_cast<ControlCommand>(data[0])) {
        case ControlCommand::LockFocus:
        case ControlCommand::UnlockFocus:
        case ControlCommand::LockExposure:
        case ControlCommand::UnlockExposure:
        case ControlCommand::RequestKeyframe:
        case ControlCommand::SetQualityLow:
        case ControlCommand::SetQualityMedium:
        case ControlCommand::SetQualityHigh:
        case ControlCommand::SetAudioEnabled:
        case ControlCommand::SetCenterStageEnabled:
        case ControlCommand::Ping:
        case ControlCommand::Pong:
            pkt.command = static_cast<ControlCommand>(data[0]);
            break;
        default:
            return std::nullopt;
        }
        if (length > 1)
            pkt.payload.assign(data + 1, data + length);
        return pkt;
    }
};

} // namespace cams

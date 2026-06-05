// FrameReassembler.h
// Cams OBS Plugin — encoded-frame fragment reassembly.

#pragma once

#include "CamsPacket.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

namespace cams {

struct ReassembledPacket {
    PacketHeader header{};
    std::vector<uint8_t> payload;
};

struct ReassemblyOutcome {
    std::optional<ReassembledPacket> packet;
    std::vector<const char *> lossReasons;
    bool accepted = false;
};

class FrameReassembler {
public:
    /// Callback invoked once per timed-out frame when `expire()` discards it.
    using LossCallback = std::function<void()>;

    ReassemblyOutcome push(PacketHeader header, std::vector<uint8_t> payload, uint64_t nowMs);
    std::vector<const char *> expire(uint64_t nowMs);
    size_t pendingFrameCount() const { return m_assemblyMap.size(); }

    /// Registers a callback fired for each frame dropped by reassembly timeout.
    void setLossCallback(LossCallback cb) { m_lossCallback = std::move(cb); }

private:
    struct FrameKey {
        uint32_t sequenceNumber = 0;
        uint64_t timestamp = 0;
        FrameType frameType = FrameType::H264;

        bool operator==(const FrameKey &other) const {
            return sequenceNumber == other.sequenceNumber &&
                   timestamp == other.timestamp &&
                   frameType == other.frameType;
        }
    };

    struct FrameKeyHash {
        size_t operator()(const FrameKey &key) const {
            size_t seed = static_cast<size_t>(key.sequenceNumber);
            seed ^= static_cast<size_t>(key.timestamp) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            seed ^= static_cast<size_t>(key.timestamp >> 32) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            seed ^= static_cast<size_t>(key.frameType) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            return seed;
        }
    };

    struct FrameAssembly {
        uint64_t receiveTimeMs = 0;
        uint16_t fragmentCount = 0;
        uint16_t fragmentsReceived = 0;
        size_t totalExpectedLength = 0;
        std::vector<std::optional<std::vector<uint8_t>>> fragments;
        PacketHeader header{};
    };

    static constexpr uint64_t kFragmentTimeoutMs = 1000;
    static constexpr size_t kMaxReassembledFrameSize = 16 * 1024 * 1024;

    LossCallback m_lossCallback;
    std::unordered_map<FrameKey, FrameAssembly, FrameKeyHash> m_assemblyMap;

    static FrameKey keyFor(const PacketHeader &header);
};

} // namespace cams

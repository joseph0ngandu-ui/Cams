// FrameReassembler.cpp
// Cams OBS Plugin — encoded-frame fragment reassembly.

#include "FrameReassembler.h"

#include <utility>

namespace cams {

ReassemblyOutcome FrameReassembler::push(
    PacketHeader header,
    std::vector<uint8_t> payload,
    uint64_t nowMs
) {
    ReassemblyOutcome outcome;
    outcome.lossReasons = expire(nowMs);

    if (header.payloadLength != payload.size()) {
        outcome.lossReasons.push_back("payload length mismatch");
        return outcome;
    }

    if (header.fragmentCount <= 1) {
        header.fragmentCount = 1;
        header.fragmentIndex = 0;
        outcome.packet = ReassembledPacket{header, std::move(payload)};
        outcome.accepted = true;
        return outcome;
    }

    const FrameKey key = keyFor(header);
    auto &assembly = m_assemblyMap[key];
    if (assembly.fragments.empty()) {
        assembly.receiveTimeMs = nowMs;
        assembly.fragmentCount = header.fragmentCount;
        assembly.fragments.resize(header.fragmentCount);
        assembly.header = header;
    } else if (assembly.fragmentCount != header.fragmentCount) {
        m_assemblyMap.erase(key);
        outcome.lossReasons.push_back("fragment metadata changed");
        return outcome;
    } else {
        assembly.receiveTimeMs = nowMs;
    }

    if (assembly.totalExpectedLength + header.payloadLength > kMaxReassembledFrameSize) {
        m_assemblyMap.erase(key);
        outcome.lossReasons.push_back("reassembled frame too large");
        return outcome;
    }

    if (assembly.fragments[header.fragmentIndex].has_value()) {
        outcome.accepted = true;
        return outcome;
    }

    assembly.fragments[header.fragmentIndex] = std::move(payload);
    assembly.fragmentsReceived++;
    assembly.totalExpectedLength += header.payloadLength;
    outcome.accepted = true;

    if (assembly.fragmentsReceived != assembly.fragmentCount) {
        return outcome;
    }

    std::vector<uint8_t> fullPayload;
    fullPayload.reserve(assembly.totalExpectedLength);
    for (auto &fragment : assembly.fragments) {
        if (!fragment.has_value()) {
            return outcome;
        }
        fullPayload.insert(fullPayload.end(), fragment->begin(), fragment->end());
    }

    PacketHeader fullHeader = assembly.header;
    fullHeader.payloadLength = static_cast<uint32_t>(fullPayload.size());
    fullHeader.fragmentCount = 1;
    fullHeader.fragmentIndex = 0;

    m_assemblyMap.erase(key);
    outcome.packet = ReassembledPacket{fullHeader, std::move(fullPayload)};
    return outcome;
}

std::vector<const char *> FrameReassembler::expire(uint64_t nowMs) {
    std::vector<const char *> lossReasons;
    for (auto it = m_assemblyMap.begin(); it != m_assemblyMap.end(); ) {
        if (nowMs >= it->second.receiveTimeMs &&
            nowMs - it->second.receiveTimeMs > kFragmentTimeoutMs) {
            lossReasons.push_back("fragment reassembly timeout");
            it = m_assemblyMap.erase(it);
        } else {
            ++it;
        }
    }
    return lossReasons;
}

FrameReassembler::FrameKey FrameReassembler::keyFor(const PacketHeader &header) {
    return {header.sequenceNumber, header.timestamp, header.frameType};
}

} // namespace cams

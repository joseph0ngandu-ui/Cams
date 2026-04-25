// JitterBufferTest.cpp
// Standalone unit tests for cams::JitterBuffer — no OBS required.
// Compile with: cmake -DBUILD_TESTS=ON and run cams_tests.

#include "ControlServer.h"
#include "FrameReassembler.h"
#include "JitterBuffer.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <thread>

// ---------------------------------------------------------------------------
// Minimal test harness
// ---------------------------------------------------------------------------

static int  g_passed = 0;
static int  g_failed = 0;

#define ASSERT_EQ(a, b)  do { \
    if ((a) != (b)) { \
        fprintf(stderr, "  FAIL %s:%d  expected %lld got %lld\n", \
            __FILE__, __LINE__, (long long)(b), (long long)(a)); \
        ++g_failed; \
    } else { ++g_passed; } \
} while (0)

#define ASSERT_TRUE(x)  do { \
    if (!(x)) { \
        fprintf(stderr, "  FAIL %s:%d  expression was false\n", __FILE__, __LINE__); \
        ++g_failed; \
    } else { ++g_passed; } \
} while (0)

#define ASSERT_FALSE(x) ASSERT_TRUE(!(x))

#define TEST(name)  static void name()
#define RUN(name)   do { \
    fprintf(stdout, "[ RUN ] " #name "\n"); \
    name(); \
    fprintf(stdout, "[%s] " #name "\n", (g_failed == prevFailed ? " OK " : "FAIL")); \
} while(0)

// Helper: builds a PacketHeader with the given sequence number.
static cams::PacketHeader makeHeader(uint32_t seq) {
    return {seq, static_cast<uint64_t>(seq) * 1000, cams::FrameType::H264, 0, 1, 4};
}

static std::vector<uint8_t> makePayload(uint32_t seq) {
    return {
        static_cast<uint8_t>((seq >> 24) & 0xFF),
        static_cast<uint8_t>((seq >> 16) & 0xFF),
        static_cast<uint8_t>((seq >>  8) & 0xFF),
        static_cast<uint8_t>( seq        & 0xFF)
    };
}

static cams::PacketHeader makeFragment(
    uint32_t seq,
    uint64_t timestamp,
    cams::FrameType frameType,
    uint16_t fragmentIndex,
    uint16_t fragmentCount,
    size_t payloadLength
) {
    return {
        seq,
        timestamp,
        frameType,
        fragmentIndex,
        fragmentCount,
        static_cast<uint32_t>(payloadLength)
    };
}

static std::vector<uint8_t> tokenPayload(uint64_t token) {
    std::vector<uint8_t> out(8);
    for (int i = 7; i >= 0; --i) {
        out[static_cast<size_t>(7 - i)] = static_cast<uint8_t>((token >> (i * 8)) & 0xFF);
    }
    return out;
}

static void sendControlPacketToLocalhost(uint16_t port, const cams::ControlPacket &packet) {
    socket_t sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCK) return;

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &dest.sin_addr);

    auto data = packet.serialise();
    ::sendto(sock,
             reinterpret_cast<const char *>(data.data()),
             static_cast<int>(data.size()),
             0,
             reinterpret_cast<const sockaddr *>(&dest),
             sizeof(dest));
    CLOSE_SOCK(sock);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(test_passthrough_mode) {
    cams::JitterBuffer buf(0);

    for (uint32_t i = 0; i < 5; ++i) {
        buf.push(makeHeader(i), makePayload(i));
    }

    for (uint32_t i = 0; i < 5; ++i) {
        auto f = buf.tryPop();
        ASSERT_TRUE(f.has_value());
        ASSERT_EQ(f->sequenceNumber, i);
    }
}

TEST(test_in_order_frames) {
    cams::JitterBuffer buf(3);

    // Push 5 frames in order.
    for (uint32_t i = 0; i < 5; ++i) {
        buf.push(makeHeader(i), makePayload(i));
    }

    // All 5 should be available in order.
    for (uint32_t i = 0; i < 5; ++i) {
        auto f = buf.tryPop();
        ASSERT_TRUE(f.has_value());
        ASSERT_EQ(f->sequenceNumber, i);
    }

    // Buffer should be empty now.
    ASSERT_TRUE(!buf.tryPop().has_value());
}

TEST(test_out_of_order_reordering) {
    cams::JitterBuffer buf(3);

    // Push frames out of order: 2, 0, 1
    buf.push(makeHeader(2), makePayload(2));
    buf.push(makeHeader(0), makePayload(0));
    buf.push(makeHeader(1), makePayload(1));

    // After 3 pushes the buffer is at capacity and should have drained.
    // We expect frames 0, 1, 2 in order.
    auto f0 = buf.tryPop();
    auto f1 = buf.tryPop();
    auto f2 = buf.tryPop();

    ASSERT_TRUE(f0.has_value());
    ASSERT_TRUE(f1.has_value());
    ASSERT_TRUE(f2.has_value());
    ASSERT_EQ(f0->sequenceNumber, 0u);
    ASSERT_EQ(f1->sequenceNumber, 1u);
    ASSERT_EQ(f2->sequenceNumber, 2u);
}

TEST(test_overflow_evicts_oldest) {
    cams::JitterBuffer buf(2);

    // Push 4 frames: 3, 0, 1, 2  — all out of order initially.
    buf.push(makeHeader(3), makePayload(3));
    buf.push(makeHeader(0), makePayload(0));
    buf.push(makeHeader(1), makePayload(1));
    // At this point the buffer of size 2 must have evicted something.
    buf.push(makeHeader(2), makePayload(2));

    // Collect all available frames — must be in ascending sequence order.
    uint32_t lastSeq = UINT32_MAX;
    bool ascending = true;
    while (auto f = buf.tryPop()) {
        if (lastSeq != UINT32_MAX && f->sequenceNumber < lastSeq) {
            ascending = false;
        }
        lastSeq = f->sequenceNumber;
    }
    ASSERT_TRUE(ascending);
}

TEST(test_flush_wakes_blocked_pop) {
    cams::JitterBuffer buf(3);

    bool woken = false;
    std::thread consumer([&] {
        auto result = buf.pop();
        woken = !result.has_value();  // flush returns nullopt
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    buf.flush();
    consumer.join();

    ASSERT_TRUE(woken);
}

TEST(test_set_capacity_flushes) {
    cams::JitterBuffer buf(3);

    buf.push(makeHeader(0), makePayload(0));
    ASSERT_EQ(buf.size(), 1u);

    buf.setCapacity(0);
    ASSERT_EQ(buf.size(), 0u);
    ASSERT_EQ(buf.capacity(), 0u);
}

TEST(test_sequence_wraparound_reordering) {
    cams::JitterBuffer buf(3);

    buf.push(makeHeader(UINT32_MAX), makePayload(UINT32_MAX));
    buf.push(makeHeader(UINT32_MAX - 1), makePayload(UINT32_MAX - 1));
    buf.push(makeHeader(0), makePayload(0));
    buf.push(makeHeader(1), makePayload(1));

    auto f0 = buf.tryPop();
    auto f1 = buf.tryPop();
    auto f2 = buf.tryPop();
    auto f3 = buf.tryPop();

    ASSERT_TRUE(f0.has_value());
    ASSERT_TRUE(f1.has_value());
    ASSERT_TRUE(f2.has_value());
    ASSERT_TRUE(f3.has_value());
    ASSERT_EQ(f0->sequenceNumber, UINT32_MAX - 1);
    ASSERT_EQ(f1->sequenceNumber, UINT32_MAX);
    ASSERT_EQ(f2->sequenceNumber, 0u);
    ASSERT_EQ(f3->sequenceNumber, 1u);
}

TEST(test_frame_gap_skips_missing_sequence_under_capacity_pressure) {
    cams::JitterBuffer buf(3);

    buf.push(makeHeader(0), makePayload(0));
    buf.push(makeHeader(2), makePayload(2));
    buf.push(makeHeader(3), makePayload(3));

    auto f0 = buf.tryPop();
    ASSERT_TRUE(f0.has_value());
    ASSERT_EQ(f0->sequenceNumber, 0u);
    ASSERT_FALSE(buf.tryPop().has_value());

    buf.push(makeHeader(4), makePayload(4));
    auto f2 = buf.tryPop();
    auto f3 = buf.tryPop();
    auto f4 = buf.tryPop();

    ASSERT_TRUE(f2.has_value());
    ASSERT_TRUE(f3.has_value());
    ASSERT_TRUE(f4.has_value());
    ASSERT_EQ(f2->sequenceNumber, 2u);
    ASSERT_EQ(f3->sequenceNumber, 3u);
    ASSERT_EQ(f4->sequenceNumber, 4u);
}

TEST(test_produce_consume_threaded) {
    cams::JitterBuffer buf(3);
    static constexpr uint32_t kCount = 50;

    std::thread producer([&] {
        for (uint32_t i = 0; i < kCount; ++i) {
            buf.push(makeHeader(i), makePayload(i));
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    std::vector<uint32_t> received;
    received.reserve(kCount);
    for (uint32_t i = 0; i < kCount; ++i) {
        auto f = buf.pop();
        if (f) received.push_back(f->sequenceNumber);
    }
    producer.join();

    // All frames should have been received (some reordering tolerated).
    ASSERT_EQ(received.size(), kCount);

    // Verify they arrived in ascending order.
    for (size_t i = 1; i < received.size(); ++i) {
        ASSERT_TRUE(received[i] >= received[i - 1]);
    }
}

TEST(test_packet_header_round_trip_with_fragment_fields) {
    cams::PacketHeader header = makeFragment(
        12,
        345,
        cams::FrameType::HEVC,
        2,
        4,
        1200
    );

    uint8_t bytes[cams::kHeaderSize] = {};
    header.serialise(bytes);
    auto parsed = cams::PacketHeader::deserialise(bytes, cams::kHeaderSize);

    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->sequenceNumber, 12u);
    ASSERT_EQ(parsed->timestamp, 345u);
    ASSERT_TRUE(parsed->frameType == cams::FrameType::HEVC);
    ASSERT_EQ(parsed->fragmentIndex, 2u);
    ASSERT_EQ(parsed->fragmentCount, 4u);
    ASSERT_EQ(parsed->payloadLength, 1200u);
}

TEST(test_packet_header_rejects_excessive_fragment_count) {
    cams::PacketHeader header = makeFragment(
        12,
        345,
        cams::FrameType::HEVC,
        0,
        cams::kMaxFragments + 1,
        1200
    );

    uint8_t bytes[cams::kHeaderSize] = {};
    header.serialise(bytes);
    auto parsed = cams::PacketHeader::deserialise(bytes, cams::kHeaderSize);

    ASSERT_FALSE(parsed.has_value());
}

TEST(test_reassembler_completes_out_of_order_fragments) {
    cams::FrameReassembler reassembler;
    auto second = std::vector<uint8_t>{3, 4};
    auto first = std::vector<uint8_t>{1, 2};

    auto pending = reassembler.push(
        makeFragment(42, 99, cams::FrameType::H264, 1, 2, second.size()),
        second,
        10
    );
    ASSERT_FALSE(pending.packet.has_value());

    auto complete = reassembler.push(
        makeFragment(42, 99, cams::FrameType::H264, 0, 2, first.size()),
        first,
        11
    );

    ASSERT_TRUE(complete.packet.has_value());
    ASSERT_EQ(complete.packet->header.sequenceNumber, 42u);
    ASSERT_EQ(complete.packet->header.fragmentIndex, 0u);
    ASSERT_EQ(complete.packet->header.fragmentCount, 1u);
    ASSERT_EQ(complete.packet->payload.size(), 4u);
    ASSERT_EQ(complete.packet->payload[0], 1u);
    ASSERT_EQ(complete.packet->payload[3], 4u);
}

TEST(test_reassembler_ignores_duplicate_fragments) {
    cams::FrameReassembler reassembler;
    auto first = std::vector<uint8_t>{1, 2};
    auto second = std::vector<uint8_t>{3, 4};

    auto firstHeader = makeFragment(42, 99, cams::FrameType::H264, 0, 2, first.size());
    reassembler.push(firstHeader, first, 10);
    auto duplicate = reassembler.push(firstHeader, first, 11);
    ASSERT_FALSE(duplicate.packet.has_value());

    auto complete = reassembler.push(
        makeFragment(42, 99, cams::FrameType::H264, 1, 2, second.size()),
        second,
        12
    );

    ASSERT_TRUE(complete.packet.has_value());
    ASSERT_EQ(complete.packet->payload.size(), 4u);
}

TEST(test_reassembler_rejects_fragment_metadata_conflict) {
    cams::FrameReassembler reassembler;
    auto payload = std::vector<uint8_t>{1, 2};

    reassembler.push(
        makeFragment(42, 99, cams::FrameType::H264, 0, 2, payload.size()),
        payload,
        10
    );
    auto conflict = reassembler.push(
        makeFragment(42, 99, cams::FrameType::H264, 1, 3, payload.size()),
        payload,
        11
    );

    ASSERT_FALSE(conflict.packet.has_value());
    ASSERT_EQ(conflict.lossReasons.size(), 1u);
    ASSERT_EQ(reassembler.pendingFrameCount(), 0u);
}

TEST(test_reassembler_expires_stale_fragments) {
    cams::FrameReassembler reassembler;
    auto payload = std::vector<uint8_t>{1, 2};

    reassembler.push(
        makeFragment(42, 99, cams::FrameType::H264, 0, 2, payload.size()),
        payload,
        10
    );
    auto reasons = reassembler.expire(1011);

    ASSERT_EQ(reasons.size(), 1u);
    ASSERT_EQ(reassembler.pendingFrameCount(), 0u);
}

TEST(test_reassembler_rejects_payload_length_mismatch) {
    cams::FrameReassembler reassembler;
    auto payload = std::vector<uint8_t>{1, 2};

    auto outcome = reassembler.push(
        makeFragment(42, 99, cams::FrameType::H264, 0, 2, 99),
        payload,
        10
    );

    ASSERT_FALSE(outcome.packet.has_value());
    ASSERT_EQ(outcome.lossReasons.size(), 1u);
    ASSERT_EQ(reassembler.pendingFrameCount(), 0u);
}

TEST(test_control_ping_receives_matching_pong) {
    cams::ControlServer sender;
    cams::ControlServer receiver;

    ASSERT_TRUE(sender.start(0));
    ASSERT_TRUE(receiver.start(0));
    sender.setTarget("127.0.0.1", receiver.boundPort());

    auto rtt = sender.ping(500);

    ASSERT_TRUE(rtt.has_value());
    ASSERT_TRUE(*rtt >= 0.0);

    sender.stop();
    receiver.stop();
}

TEST(test_control_ping_ignores_mismatched_pong_token) {
    cams::ControlServer sender;
    ASSERT_TRUE(sender.start(0));

    std::thread wrongPong([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        cams::ControlPacket packet{cams::ControlCommand::Pong, tokenPayload(999)};
        sendControlPacketToLocalhost(sender.boundPort(), packet);
    });

    auto rtt = sender.ping(80);

    wrongPong.join();
    ASSERT_FALSE(rtt.has_value());
    sender.stop();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    fprintf(stdout, "=== Cams JitterBuffer Tests ===\n");

    int prevFailed = 0;
    RUN(test_passthrough_mode);       prevFailed = g_failed;
    RUN(test_in_order_frames);        prevFailed = g_failed;
    RUN(test_out_of_order_reordering); prevFailed = g_failed;
    RUN(test_overflow_evicts_oldest); prevFailed = g_failed;
    RUN(test_flush_wakes_blocked_pop); prevFailed = g_failed;
    RUN(test_set_capacity_flushes);   prevFailed = g_failed;
    RUN(test_sequence_wraparound_reordering); prevFailed = g_failed;
    RUN(test_frame_gap_skips_missing_sequence_under_capacity_pressure); prevFailed = g_failed;
    RUN(test_produce_consume_threaded); prevFailed = g_failed;
    RUN(test_packet_header_round_trip_with_fragment_fields); prevFailed = g_failed;
    RUN(test_packet_header_rejects_excessive_fragment_count); prevFailed = g_failed;
    RUN(test_reassembler_completes_out_of_order_fragments); prevFailed = g_failed;
    RUN(test_reassembler_ignores_duplicate_fragments); prevFailed = g_failed;
    RUN(test_reassembler_rejects_fragment_metadata_conflict); prevFailed = g_failed;
    RUN(test_reassembler_expires_stale_fragments); prevFailed = g_failed;
    RUN(test_reassembler_rejects_payload_length_mismatch); prevFailed = g_failed;
    RUN(test_control_ping_receives_matching_pong); prevFailed = g_failed;
    RUN(test_control_ping_ignores_mismatched_pong_token); prevFailed = g_failed;

    fprintf(stdout, "\nResults: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

// JitterBufferTest.cpp
// Standalone unit tests for cams::JitterBuffer — no OBS required.
// Compile with: cmake -DBUILD_TESTS=ON and run cams_tests.

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
    RUN(test_produce_consume_threaded); prevFailed = g_failed;

    fprintf(stdout, "\nResults: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

// JitterBuffer.cpp
// Cams OBS Plugin — Fixed-latency jitter buffer implementation.

#include "JitterBuffer.h"

#include <algorithm>
#include <cassert>

namespace cams {

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

JitterBuffer::JitterBuffer(size_t capacity)
    : m_capacity(capacity)
{}

JitterBuffer::~JitterBuffer() {
    flush();
}

// ---------------------------------------------------------------------------
// Producer
// ---------------------------------------------------------------------------

void JitterBuffer::push(PacketHeader header, std::vector<uint8_t> payload) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Zero-capacity (pass-through) mode — go directly to ready queue.
        if (m_capacity == 0) {
            m_ready.push_back({header.sequenceNumber, header.timestamp,
                               header.frameType, std::move(payload)});
        } else {
            // Fast path: if we have established the expected sequence and this
            // frame arrives in order, emit it directly.
            if (m_started && header.sequenceNumber == m_nextExpected) {
                m_ready.push_back({header.sequenceNumber, header.timestamp,
                                   header.frameType, std::move(payload)});
                ++m_nextExpected;
                drainPending();
            } else {
                // Out-of-order or pre-establishment path.
                Entry entry{header, std::move(payload)};
                auto it = std::lower_bound(
                    m_pending.begin(), m_pending.end(), entry,
                    [](const Entry &a, const Entry &b) {
                        return a.header.sequenceNumber < b.header.sequenceNumber;
                    }
                );
                m_pending.insert(it, std::move(entry));

                // When pending reaches capacity, forcibly evict the oldest frame
                // to prevent consumer starvation.
                while (m_pending.size() >= m_capacity) {
                    auto &oldest = m_pending.front();
                    m_ready.push_back({
                        oldest.header.sequenceNumber,
                        oldest.header.timestamp,
                        oldest.header.frameType,
                        std::move(oldest.payload)
                    });
                    m_nextExpected = oldest.header.sequenceNumber + 1;
                    m_started = true;
                    m_pending.pop_front();
                }

                // After possible eviction, drain any in-order frames.
                if (m_started) {
                    drainPending();
                }
            }
        }
    }
    m_cv.notify_one();
}

// ---------------------------------------------------------------------------
// Consumer
// ---------------------------------------------------------------------------

std::optional<DecodedFrame> JitterBuffer::pop() {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [this] {
        return !m_ready.empty() || m_flushed;
    });
    if (m_flushed && m_ready.empty()) return std::nullopt;
    return dequeueReady();
}

std::optional<DecodedFrame> JitterBuffer::tryPop() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_ready.empty()) return std::nullopt;
    return dequeueReady();
}

// ---------------------------------------------------------------------------
// Control
// ---------------------------------------------------------------------------

void JitterBuffer::flush() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending.clear();
        m_ready.clear();
        m_flushed      = true;
        m_started      = false;
        m_nextExpected = 0;
    }
    m_cv.notify_all();
}

void JitterBuffer::setCapacity(size_t newCapacity) {
    flush();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_capacity = newCapacity;
    m_flushed  = false;
}

size_t JitterBuffer::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_ready.size() + m_pending.size();
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void JitterBuffer::drainPending() {
    // Release frames whose sequence number exactly matches m_nextExpected.
    // Caller must hold m_mutex.
    while (!m_pending.empty()) {
        auto &front = m_pending.front();
        if (front.header.sequenceNumber != m_nextExpected) break;

        m_ready.push_back({
            front.header.sequenceNumber,
            front.header.timestamp,
            front.header.frameType,
            std::move(front.payload)
        });
        m_pending.pop_front();
        ++m_nextExpected;
    }
}

DecodedFrame JitterBuffer::dequeueReady() {
    // Caller must hold m_mutex.
    DecodedFrame frame = std::move(m_ready.front());
    m_ready.pop_front();
    return frame;
}

} // namespace cams

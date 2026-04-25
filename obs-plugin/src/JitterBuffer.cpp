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
        m_flushed = false;

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
            } else if (m_started && sequenceBefore(header.sequenceNumber, m_nextExpected)) {
                return;
            } else {
                // Out-of-order or pre-establishment path.
                auto duplicate = std::find_if(
                    m_pending.begin(), m_pending.end(),
                    [&header](const Entry &entry) {
                        return entry.header.sequenceNumber == header.sequenceNumber;
                    }
                );
                if (duplicate != m_pending.end()) return;

                Entry entry{header, std::move(payload)};
                auto it = std::lower_bound(
                    m_pending.begin(), m_pending.end(), entry,
                    [](const Entry &a, const Entry &b) {
                        return JitterBuffer::sequenceBefore(a.header.sequenceNumber, b.header.sequenceNumber);
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
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Clear pending/ready without setting m_flushed — that flag is
        // reserved for pipeline shutdown and would permanently kill the
        // decoder thread.
        m_pending.clear();
        m_ready.clear();
        m_started      = false;
        m_nextExpected = 0;
        m_capacity     = newCapacity;
    }
    // Wake the consumer so it can re-evaluate; m_flushed is still false,
    // so pop() will simply loop back and wait for new data.
    m_cv.notify_all();
}

size_t JitterBuffer::capacity() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_capacity;
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

bool JitterBuffer::sequenceBefore(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) < 0;
}

} // namespace cams

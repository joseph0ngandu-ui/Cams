// JitterBuffer.h
// Cams OBS Plugin — Fixed-latency jitter buffer for UDP packet reordering.
//
// Holds exactly N frames (configurable: 0 = low-latency, 3 = stable).
// Frames are stored by sequence number, reordered, and released in order
// when the ring is full or on a configurable timeout.

#pragma once

#include "CamsPacket.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace cams {

// ---------------------------------------------------------------------------
// DecodedFrame
// ---------------------------------------------------------------------------

/// A reassembled, presentation-ordered video frame ready for the OBS pipeline.
struct DecodedFrame {
    uint32_t             sequenceNumber;
    uint64_t             timestamp;        ///< Capture timestamp (ns).
    FrameType            frameType;
    std::vector<uint8_t> data;             ///< Encoded payload bytes.
};

// ---------------------------------------------------------------------------
// JitterBuffer
// ---------------------------------------------------------------------------

/// Thread-safe fixed-latency jitter buffer.
///
/// The network listener thread pushes raw packets with `push()`.
/// The OBS video thread consumes ordered frames with `pop()`.
///
/// Capacity:
///   0 — pass-through (no reordering), minimum latency.
///   3 — buffer up to 3 frames for reordering.
class JitterBuffer {
public:
    /// Construct a jitter buffer with `capacity` frame slots.
    explicit JitterBuffer(size_t capacity = 3);

    /// Destructor — wakes any blocked `pop()` calls.
    ~JitterBuffer();

    // -----------------------------------------------------------------------
    // Producer API (network listener thread)

    /// Inserts an incoming packet into the buffer.
    ///
    /// If the buffer is full the oldest out-of-order frame is evicted to make
    /// room, preventing the network listener from stalling.
    void push(PacketHeader header, std::vector<uint8_t> payload);

    // -----------------------------------------------------------------------
    // Consumer API (OBS video thread)

    /// Blocks until a frame is available or the buffer is flushed.
    ///
    /// Returns std::nullopt when the buffer is flushed/destroyed.
    std::optional<DecodedFrame> pop();

    /// Non-blocking attempt to pop a frame.
    std::optional<DecodedFrame> tryPop();

    // -----------------------------------------------------------------------
    // Control

    /// Flush all pending frames and wake any blocked `pop()` callers.
    void flush();

    /// Change the jitter-buffer capacity at runtime (flushes current content).
    void setCapacity(size_t newCapacity);

    size_t capacity() const { return m_capacity; }
    size_t size() const;

private:
    // Pending entry — may arrive out of order.
    struct Entry {
        PacketHeader         header;
        std::vector<uint8_t> payload;
    };

    size_t                   m_capacity;
    uint32_t                 m_nextExpected = 0;
    bool                     m_started      = false;
    bool                     m_flushed      = false;

    std::deque<Entry>        m_pending;   ///< Out-of-order holding area.
    std::deque<DecodedFrame> m_ready;     ///< Ordered, ready-to-output frames.

    mutable std::mutex       m_mutex;
    std::condition_variable  m_cv;

    // Releases any in-order frames from m_pending into m_ready.
    // Caller must hold m_mutex.
    void drainPending();

    // Removes and returns the front of m_ready.
    // Caller must hold m_mutex and m_ready must be non-empty.
    DecodedFrame dequeueReady();
};

} // namespace cams

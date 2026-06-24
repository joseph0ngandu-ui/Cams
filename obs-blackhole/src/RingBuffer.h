/*
 * RingBuffer.h — single-producer / single-consumer lock-free float ring.
 *
 * Producer: OBS audio thread (raw mix callback).
 * Consumer: CoreAudio HAL render thread.
 *
 * Stores interleaved stereo float samples. Both sides are wait-free; the only
 * shared state is two atomic indices with acquire/release ordering.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

class RingBuffer {
public:
	// capacityFrames is rounded to hold capacityFrames * channels floats.
	void init(size_t capacityFrames, size_t channels)
	{
		channels_ = channels;
		capacity_ = (capacityFrames * channels) + 1; // +1 to distinguish full/empty
		buffer_.assign(capacity_, 0.0f);
		read_.store(0, std::memory_order_relaxed);
		write_.store(0, std::memory_order_relaxed);
	}

	void clear()
	{
		read_.store(0, std::memory_order_relaxed);
		write_.store(0, std::memory_order_relaxed);
	}

	size_t channels() const { return channels_; }

	// Producer side: write up to `count` floats; returns floats actually written.
	size_t write(const float *src, size_t count)
	{
		const size_t w = write_.load(std::memory_order_relaxed);
		const size_t r = read_.load(std::memory_order_acquire);
		size_t free_space = (r + capacity_ - w - 1) % capacity_;
		if (count > free_space)
			count = free_space; // drop the overflow rather than block
		for (size_t i = 0; i < count; ++i)
			buffer_[(w + i) % capacity_] = src[i];
		write_.store((w + count) % capacity_, std::memory_order_release);
		return count;
	}

	// Consumer side: read exactly `count` floats, zero-filling any underrun.
	// Returns floats that were real (non-silence).
	size_t readOrSilence(float *dst, size_t count)
	{
		const size_t r = read_.load(std::memory_order_relaxed);
		const size_t w = write_.load(std::memory_order_acquire);
		size_t avail = (w + capacity_ - r) % capacity_;
		size_t real = (count < avail) ? count : avail;
		for (size_t i = 0; i < real; ++i)
			dst[i] = buffer_[(r + i) % capacity_];
		for (size_t i = real; i < count; ++i)
			dst[i] = 0.0f; // underrun -> silence
		read_.store((r + real) % capacity_, std::memory_order_release);
		return real;
	}

	size_t availableFrames() const
	{
		const size_t r = read_.load(std::memory_order_acquire);
		const size_t w = write_.load(std::memory_order_acquire);
		return ((w + capacity_ - r) % capacity_) / (channels_ ? channels_ : 1);
	}

private:
	std::vector<float> buffer_;
	size_t capacity_ = 1;
	size_t channels_ = 2;
	std::atomic<size_t> read_{0};
	std::atomic<size_t> write_{0};
};

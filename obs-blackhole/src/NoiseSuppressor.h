/*
 * NoiseSuppressor.h — RNNoise wrapper for the virtual-mic chain.
 *
 * RNNoise is a neural denoiser fixed at 48 kHz, mono, 480-sample (10 ms)
 * frames. OBS hands us interleaved stereo at a variable frame count (~1024),
 * so this class de-interleaves, buffers per channel into 480-sample blocks,
 * runs each block through a per-channel DenoiseState, and re-interleaves.
 *
 * Output is primed with one block of silence so the number of samples returned
 * always matches the number passed in (introducing ~10 ms of latency).
 *
 * Scaling mirrors OBS's own noise-suppress filter: RNNoise expects float
 * samples in int16 range, so we multiply by 32768 in and divide by 32768 out.
 *
 * Lives entirely on the OBS audio thread; not thread-safe by itself.
 */
#pragma once

#include <cstdint>
#include <vector>

struct DenoiseState; // fwd (rnnoise.h)

class NoiseSuppressor {
public:
	~NoiseSuppressor();

	// channels: 1 or 2. Returns false if RNNoise state creation failed.
	bool prepare(uint32_t channels);

	// Denoise `frames` samples per channel in place (interleaved float).
	void process(float *interleaved, size_t frames);

	uint32_t channels() const { return channels_; }

private:
	static constexpr int kFrame = 480;     // RNNoise frame size @ 48 kHz
	static constexpr uint32_t kMaxCh = 2;

	void destroy();

	uint32_t channels_ = 0;
	DenoiseState *states_[kMaxCh] = {nullptr, nullptr};
	std::vector<float> in_[kMaxCh];        // per-channel input FIFO
	std::vector<float> out_[kMaxCh];       // per-channel output FIFO (primed)
};

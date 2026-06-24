/*
 * AudioDsp.h — self-contained dynamics processors for the virtual-mic chain.
 *
 * Both operate in place on interleaved float (any channel count) and keep a
 * single shared gain envelope across channels so stereo imaging is preserved.
 * No external dependencies. Modeled on OBS's compressor/limiter filters.
 *
 * Run on the OBS audio thread only; not thread-safe by themselves.
 */
#pragma once

#include <cstddef>

// Brick-wall peak limiter: guarantees the signal never exceeds a ceiling
// (default -1 dBFS), so the virtual mic can't digitally clip on loud peaks.
class Limiter {
public:
	void prepare(double sampleRate);
	void reset();
	void process(float *buf, size_t frames, size_t channels);

private:
	double sampleRate_ = 48000.0;
	float ceiling_ = 0.0f;     // linear, ~-1 dBFS
	float attackCoef_ = 0.0f;  // fast
	float releaseCoef_ = 0.0f; // ~60 ms
	float gain_ = 1.0f;        // current gain reduction (1 = no reduction)
};

// Soft-knee-less peak compressor with auto makeup gain. Ships broadcast-style
// defaults (threshold -18 dB, ratio 4:1, attack 6 ms, release 60 ms) so a
// single toggle gives a consistently leveled voice.
class Compressor {
public:
	void prepare(double sampleRate);
	void reset();
	void process(float *buf, size_t frames, size_t channels);

private:
	double sampleRate_ = 48000.0;
	float thresholdDb_ = -18.0f;
	float ratio_ = 4.0f;
	float makeupDb_ = 0.0f; // computed from threshold/ratio in prepare()
	float attackCoef_ = 0.0f;
	float releaseCoef_ = 0.0f;
	float env_ = 0.0f; // linear amplitude envelope
};

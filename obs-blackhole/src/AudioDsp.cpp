#include "AudioDsp.h"

#include <cmath>

namespace {
// Time-constant coefficient for a one-pole smoother: the fraction of the old
// value retained per sample for the given time in seconds.
inline float coef(double seconds, double sampleRate)
{
	if (seconds <= 0.0)
		return 0.0f;
	return (float)std::exp(-1.0 / (seconds * sampleRate));
}
inline float dbToLin(float db)
{
	return std::pow(10.0f, db / 20.0f);
}
} // namespace

// ---- Limiter ---------------------------------------------------------------

void Limiter::prepare(double sampleRate)
{
	sampleRate_ = sampleRate > 0 ? sampleRate : 48000.0;
	ceiling_ = dbToLin(-1.0f);           // -1 dBFS
	attackCoef_ = coef(0.0015, sampleRate_);  // ~1.5 ms
	releaseCoef_ = coef(0.060, sampleRate_);  // ~60 ms
	gain_ = 1.0f;
}

void Limiter::reset()
{
	gain_ = 1.0f;
}

void Limiter::process(float *buf, size_t frames, size_t channels)
{
	if (!buf || channels == 0)
		return;
	for (size_t i = 0; i < frames; ++i) {
		float *frame = buf + i * channels;

		// Peak across channels for this sample frame.
		float peak = 0.0f;
		for (size_t c = 0; c < channels; ++c) {
			float a = std::fabs(frame[c]);
			if (a > peak)
				peak = a;
		}

		// Desired gain to keep the peak at or below the ceiling.
		float target = (peak > ceiling_) ? (ceiling_ / peak) : 1.0f;

		// Fast attack when clamping down, slow release when easing back.
		if (target < gain_)
			gain_ = target + (gain_ - target) * attackCoef_;
		else
			gain_ = target + (gain_ - target) * releaseCoef_;

		for (size_t c = 0; c < channels; ++c)
			frame[c] *= gain_;
	}
}

// ---- Compressor ------------------------------------------------------------

void Compressor::prepare(double sampleRate)
{
	sampleRate_ = sampleRate > 0 ? sampleRate : 48000.0;
	attackCoef_ = coef(0.006, sampleRate_); // 6 ms
	releaseCoef_ = coef(0.060, sampleRate_); // 60 ms
	// Auto makeup: roughly half the gain lost at the threshold, so quiet
	// passages come up without slamming the overall level.
	float reductionAtThresh = -thresholdDb_ * (1.0f - 1.0f / ratio_);
	makeupDb_ = 0.5f * reductionAtThresh;
	env_ = 0.0f;
}

void Compressor::reset()
{
	env_ = 0.0f;
}

void Compressor::process(float *buf, size_t frames, size_t channels)
{
	if (!buf || channels == 0)
		return;
	const float makeup = dbToLin(makeupDb_);
	for (size_t i = 0; i < frames; ++i) {
		float *frame = buf + i * channels;

		float peak = 0.0f;
		for (size_t c = 0; c < channels; ++c) {
			float a = std::fabs(frame[c]);
			if (a > peak)
				peak = a;
		}

		// Envelope follower (attack rising, release falling).
		if (peak > env_)
			env_ = peak + (env_ - peak) * attackCoef_;
		else
			env_ = peak + (env_ - peak) * releaseCoef_;

		float envDb = 20.0f * std::log10(env_ + 1e-9f);
		float gainDb = 0.0f;
		if (envDb > thresholdDb_)
			gainDb = (envDb - thresholdDb_) * (1.0f / ratio_ - 1.0f);

		float g = dbToLin(gainDb) * makeup;
		for (size_t c = 0; c < channels; ++c)
			frame[c] *= g;
	}
}

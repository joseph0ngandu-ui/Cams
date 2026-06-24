#include "NoiseSuppressor.h"

#include <rnnoise.h>

NoiseSuppressor::~NoiseSuppressor()
{
	destroy();
}

void NoiseSuppressor::destroy()
{
	for (uint32_t c = 0; c < kMaxCh; ++c) {
		if (states_[c]) {
			rnnoise_destroy(states_[c]);
			states_[c] = nullptr;
		}
		in_[c].clear();
		out_[c].clear();
	}
	channels_ = 0;
}

bool NoiseSuppressor::prepare(uint32_t channels)
{
	destroy();
	if (channels < 1)
		channels = 1;
	if (channels > kMaxCh)
		channels = kMaxCh;

	for (uint32_t c = 0; c < channels; ++c) {
		states_[c] = rnnoise_create(nullptr); // built-in model
		if (!states_[c]) {
			destroy();
			return false;
		}
		// Reserve generously to avoid reallocation on the audio thread.
		in_[c].reserve(8192);
		out_[c].reserve(8192);
		// Prime output with one block of silence so output count always
		// matches input count (this is the ~10 ms of added latency).
		out_[c].assign(kFrame, 0.0f);
	}
	channels_ = channels;
	return true;
}

void NoiseSuppressor::process(float *interleaved, size_t frames)
{
	if (!channels_ || frames == 0 || !interleaved)
		return;

	const uint32_t ch = channels_;
	for (uint32_t c = 0; c < ch; ++c) {
		// De-interleave this channel onto its input FIFO.
		std::vector<float> &in = in_[c];
		std::vector<float> &out = out_[c];
		for (size_t i = 0; i < frames; ++i)
			in.push_back(interleaved[i * ch + c]);

		// Consume as many full 480-sample blocks as we have.
		size_t pos = 0;
		float seg[kFrame];
		while (in.size() - pos >= (size_t)kFrame) {
			for (int j = 0; j < kFrame; ++j)
				seg[j] = in[pos + j] * 32768.0f;
			rnnoise_process_frame(states_[c], seg, seg);
			for (int j = 0; j < kFrame; ++j)
				out.push_back(seg[j] / 32768.0f);
			pos += kFrame;
		}
		if (pos)
			in.erase(in.begin(), in.begin() + pos);

		// Emit `frames` denoised samples back into the interleaved buffer.
		// out is guaranteed to hold >= frames thanks to the silence prime.
		for (size_t i = 0; i < frames; ++i)
			interleaved[i * ch + c] = out[i];
		out.erase(out.begin(), out.begin() + frames);
	}
}

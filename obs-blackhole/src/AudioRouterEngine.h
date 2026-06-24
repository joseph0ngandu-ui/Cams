/*
 * AudioRouterEngine.h — VoiceMeeter-style point-to-point audio router.
 *
 * Captures a hardware input device (a microphone), runs it through the same
 * DSP chain as the virtual mic (gain/mute -> noise suppression -> compressor
 * -> limiter), and plays it out to a chosen output device (e.g. a USB mixer).
 *
 * Flow:
 *   AUHAL input unit (mic) --capture--> [DSP] --> RingBuffer
 *   AUHAL output unit (mixer) --render--> chosen output device
 *
 * The two devices run on independent clocks; the ring absorbs the small drift
 * (silence on underrun, drop on overflow). Internal format is 48 kHz Float32
 * stereo so RNNoise can run; CoreAudio converts to/from each device's rate.
 */
#pragma once

#include "RingBuffer.h"
#include "AudioDsp.h"
#include "NoiseSuppressor.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class AudioRouterEngine {
public:
	~AudioRouterEngine();

	// Resolve UIDs to devices and start routing. Returns false (logged) if a
	// device is missing or input == output (which would feed back).
	bool start(const std::string &inputUID, const std::string &outputUID);
	void stop();
	bool running() const { return running_.load(); }

	void setGainDb(double db) { gainDb_.store(db); }
	double gainDb() const { return gainDb_.load(); }
	void setMuted(bool m) { muted_.store(m); }
	bool muted() const { return muted_.load(); }

	void setNoiseSuppress(bool on) { nsEnabled_.store(on); }
	bool noiseSuppress() const { return nsEnabled_.load(); }
	void setCompressor(bool on) { compEnabled_.store(on); }
	bool compressor() const { return compEnabled_.load(); }
	void setLimiter(bool on) { limiterEnabled_.store(on); }
	bool limiter() const { return limiterEnabled_.load(); }

	// 0.0..1.0 peak since last poll (UI meter).
	float peakLevel() const { return peak_.exchange(0.0f); }

	const std::string &inputName() const { return inputName_; }
	const std::string &outputName() const { return outputName_; }

private:
	bool setupInputUnit(AudioDeviceID device);
	bool setupOutputUnit(AudioDeviceID device);
	void teardown();

	static OSStatus inputThunk(void *inRefCon, AudioUnitRenderActionFlags *flags,
				   const AudioTimeStamp *ts, UInt32 bus, UInt32 frames,
				   AudioBufferList *io);
	void onCapture(AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts,
		       UInt32 bus, UInt32 frames);

	static OSStatus renderThunk(void *inRefCon, AudioUnitRenderActionFlags *flags,
				    const AudioTimeStamp *ts, UInt32 bus, UInt32 frames,
				    AudioBufferList *io);

	RingBuffer ring_;
	AudioUnit inputUnit_ = nullptr;
	AudioUnit outputUnit_ = nullptr;

	std::unique_ptr<NoiseSuppressor> ns_;
	Compressor comp_;
	Limiter limiter_;

	std::atomic<bool> running_{false};
	std::atomic<double> gainDb_{0.0};
	std::atomic<bool> muted_{false};
	std::atomic<bool> nsEnabled_{false};
	std::atomic<bool> compEnabled_{false};
	std::atomic<bool> limiterEnabled_{false};
	mutable std::atomic<float> peak_{0.0f};

	uint32_t sampleRate_ = 48000;
	uint32_t captureChannels_ = 1; // mic native (1 or 2)
	static constexpr uint32_t kOutChannels = 2;

	// Scratch buffers for the capture render (sized to MaximumFramesPerSlice).
	std::vector<float> captureBuf_; // interleaved, captureChannels_
	std::vector<float> stereoBuf_;  // interleaved stereo for the ring
	UInt32 maxFrames_ = 4096;

	std::string inputName_;
	std::string outputName_;
};

/*
 * VirtualMicEngine.h — routes OBS's final audio mix into the virtual device.
 *
 * Flow:
 *   OBS audio thread --(obs_add_raw_audio_callback)--> RingBuffer
 *   CoreAudio render thread --(AUHAL render callback)--> "OBS Audio" device
 *
 * The device then presents that audio as a *microphone* input to every other
 * app on the system. Gain and a peak level meter are applied in the OBS tap.
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

struct audio_data; // fwd (OBS)

class VirtualMicEngine {
public:
	~VirtualMicEngine();

	// mixIndex: OBS audio track 0..5. Returns false on failure (logged).
	bool start(size_t mixIndex);
	void stop();
	bool running() const { return running_.load(); }

	void setGainDb(double db);
	double gainDb() const { return gainDb_.load(); }

	void setMuted(bool m) { muted_.store(m); }
	bool muted() const { return muted_.load(); }

	// --- Processing chain (each toggle is checked per audio callback) ----
	void setNoiseSuppress(bool on) { nsEnabled_.store(on); }
	bool noiseSuppress() const { return nsEnabled_.load(); }
	void setCompressor(bool on) { compEnabled_.store(on); }
	bool compressor() const { return compEnabled_.load(); }
	void setLimiter(bool on) { limiterEnabled_.store(on); }
	bool limiter() const { return limiterEnabled_.load(); }

	// True only when OBS runs at 48 kHz (RNNoise's fixed rate).
	bool nsSupported() const { return nsSupported_.load(); }
	uint32_t sampleRateHz() const { return sampleRate_; }

	// --- Local monitoring (hear the processed mic on a real output) ------
	// Returns false (and stays off) if monitoring couldn't start, e.g. the
	// default output IS the virtual device (would feed back).
	bool setMonitor(bool on);
	bool monitor() const { return monitorEnabled_.load(); }
	void setMonitorGainDb(double db) { monitorGainDb_.store(db); }
	double monitorGainDb() const { return monitorGainDb_.load(); }
	bool monitorWouldFeedback() const { return monitorFeedback_.load(); }
	// Empty UID = follow the system default output device.
	void setMonitorDevice(const std::string &uid) { monitorUID_ = uid; }
	const std::string &monitorDevice() const { return monitorUID_; }

	// 0.0..1.0 peak across both channels since last poll (UI meter).
	float peakLevel() const { return peak_.exchange(0.0f); }

	size_t mixIndex() const { return mixIndex_; }
	const std::string &deviceName() const { return deviceName_; }
	bool deviceMissing() const { return deviceMissing_.load(); }

private:
	bool setupAudioUnit(AudioDeviceID device, double sampleRate);
	void teardownAudioUnit();

	bool setupMonitorUnit();
	void teardownMonitorUnit();

	// OBS raw-mix callback (static trampoline + member).
	static void obsAudioThunk(void *param, size_t mix_idx, struct audio_data *data);
	void onObsAudio(struct audio_data *data);

	// CoreAudio render callbacks (static trampolines + members).
	static OSStatus renderThunk(void *inRefCon, AudioUnitRenderActionFlags *flags,
				    const AudioTimeStamp *ts, UInt32 bus, UInt32 frames,
				    AudioBufferList *io);
	static OSStatus monitorRenderThunk(void *inRefCon, AudioUnitRenderActionFlags *flags,
					   const AudioTimeStamp *ts, UInt32 bus, UInt32 frames,
					   AudioBufferList *io);

	RingBuffer ring_;
	AudioUnit outputUnit_ = nullptr;

	// Monitoring (second output unit -> user's real speakers).
	RingBuffer monitorRing_;
	AudioUnit monitorUnit_ = nullptr;

	// DSP processors (touched only on the OBS audio thread).
	std::unique_ptr<NoiseSuppressor> ns_;
	Compressor comp_;
	Limiter limiter_;

	std::atomic<bool> running_{false};
	std::atomic<bool> deviceMissing_{false};
	std::atomic<double> gainDb_{0.0};
	std::atomic<bool> muted_{false};

	std::atomic<bool> nsEnabled_{false};
	std::atomic<bool> compEnabled_{false};
	std::atomic<bool> limiterEnabled_{false};
	std::atomic<bool> nsSupported_{false};

	std::atomic<bool> monitorEnabled_{false};
	std::atomic<bool> monitorFeedback_{false};
	std::atomic<double> monitorGainDb_{0.0};
	std::string monitorUID_;                          // chosen monitor device (UI thread)
	AudioDeviceID monitorDeviceId_ = kAudioObjectUnknown; // resolved target

	mutable std::atomic<float> peak_{0.0f};

	size_t mixIndex_ = 0;
	uint32_t sampleRate_ = 48000;
	uint32_t channels_ = 2;
	std::string deviceName_;
};

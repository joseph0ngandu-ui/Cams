#include "VirtualMicEngine.h"
#include "AudioDeviceControl.h"

#include <obs.h>
#include <util/base.h>

#include <cmath>
#include <cstring>

#define ENGINE_LOG(level, fmt, ...) blog(level, "[obs-blackhole] " fmt, ##__VA_ARGS__)

VirtualMicEngine::~VirtualMicEngine()
{
	stop();
}

void VirtualMicEngine::setGainDb(double db)
{
	gainDb_.store(db);
}

bool VirtualMicEngine::start(size_t mixIndex)
{
	if (running_.load())
		return true;

	// Match OBS's audio configuration so no resampling is needed.
	obs_audio_info oai{};
	if (!obs_get_audio_info(&oai)) {
		ENGINE_LOG(LOG_WARNING, "obs_get_audio_info failed; defaulting 48k stereo");
		oai.samples_per_sec = 48000;
	}
	sampleRate_ = oai.samples_per_sec ? oai.samples_per_sec : 48000;
	channels_ = 2; // we always present a stereo virtual mic

	std::string name, uid;
	AudioDeviceID dev = AudioDeviceControl::findVirtualDevice(&name, &uid);
	if (dev == kAudioObjectUnknown) {
		deviceMissing_.store(true);
		ENGINE_LOG(LOG_WARNING,
			   "No virtual audio device live yet. Install the 'OBS Audio' "
			   "driver (or BlackHole) and reload coreaudiod, then start again.");
		return false;
	}
	deviceMissing_.store(false);
	deviceName_ = name;

	// ~500 ms of slack absorbs scheduling jitter between the two threads.
	ring_.init(sampleRate_ / 2, channels_);
	ring_.clear();
	monitorRing_.init(sampleRate_ / 2, channels_);
	monitorRing_.clear();

	// Prepare the processing chain for this sample rate.
	comp_.prepare(sampleRate_);
	limiter_.prepare(sampleRate_);
	// RNNoise is fixed at 48 kHz; only offer it when OBS matches.
	nsSupported_.store(sampleRate_ == 48000);
	ns_.reset();
	if (nsSupported_.load()) {
		ns_ = std::make_unique<NoiseSuppressor>();
		if (!ns_->prepare(channels_)) {
			ENGINE_LOG(LOG_WARNING, "RNNoise init failed; noise suppression disabled");
			ns_.reset();
			nsSupported_.store(false);
		}
	} else {
		ENGINE_LOG(LOG_INFO,
			   "Noise suppression unavailable: OBS is at %u Hz (needs 48000)",
			   sampleRate_);
	}

	if (!setupAudioUnit(dev, sampleRate_)) {
		ENGINE_LOG(LOG_ERROR, "Failed to set up CoreAudio output unit");
		teardownAudioUnit();
		return false;
	}

	mixIndex_ = mixIndex;

	struct audio_convert_info conv {};
	conv.samples_per_sec = sampleRate_;
	conv.format = AUDIO_FORMAT_FLOAT;   // interleaved float
	conv.speakers = SPEAKERS_STEREO;
	conv.allow_clipping = false;
	obs_add_raw_audio_callback(mixIndex_, &conv, obsAudioThunk, this);

	if (AudioOutputUnitStart(outputUnit_) != noErr) {
		ENGINE_LOG(LOG_ERROR, "AudioOutputUnitStart failed");
		obs_remove_raw_audio_callback(mixIndex_, obsAudioThunk, this);
		teardownAudioUnit();
		return false;
	}

	running_.store(true);
	ENGINE_LOG(LOG_INFO, "Virtual mic started -> '%s' (track %zu, %u Hz)",
		   deviceName_.c_str(), mixIndex_ + 1, sampleRate_);
	return true;
}

void VirtualMicEngine::stop()
{
	if (!running_.exchange(false))
		return;
	obs_remove_raw_audio_callback(mixIndex_, obsAudioThunk, this);
	if (outputUnit_)
		AudioOutputUnitStop(outputUnit_);
	teardownAudioUnit();
	teardownMonitorUnit();
	monitorEnabled_.store(false);
	ns_.reset();
	ENGINE_LOG(LOG_INFO, "Virtual mic stopped");
}

bool VirtualMicEngine::setupAudioUnit(AudioDeviceID device, double sampleRate)
{
	AudioComponentDescription desc{};
	desc.componentType = kAudioUnitType_Output;
	desc.componentSubType = kAudioUnitSubType_HALOutput;
	desc.componentManufacturer = kAudioUnitManufacturer_Apple;

	AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
	if (!comp)
		return false;
	if (AudioComponentInstanceNew(comp, &outputUnit_) != noErr || !outputUnit_)
		return false;

	// Enable output (bus 0), disable input (bus 1).
	UInt32 enable = 1, disable = 0;
	AudioUnitSetProperty(outputUnit_, kAudioOutputUnitProperty_EnableIO,
			     kAudioUnitScope_Output, 0, &enable, sizeof(enable));
	AudioUnitSetProperty(outputUnit_, kAudioOutputUnitProperty_EnableIO,
			     kAudioUnitScope_Input, 1, &disable, sizeof(disable));

	// Bind to the target (virtual) device.
	if (AudioUnitSetProperty(outputUnit_, kAudioOutputUnitProperty_CurrentDevice,
				 kAudioUnitScope_Global, 0, &device, sizeof(device)) != noErr)
		return false;

	// Tell the unit the format we will hand it: interleaved Float32 stereo.
	AudioStreamBasicDescription fmt{};
	fmt.mSampleRate = sampleRate;
	fmt.mFormatID = kAudioFormatLinearPCM;
	fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
	fmt.mChannelsPerFrame = channels_;
	fmt.mBitsPerChannel = 32;
	fmt.mFramesPerPacket = 1;
	fmt.mBytesPerFrame = sizeof(float) * channels_;
	fmt.mBytesPerPacket = fmt.mBytesPerFrame;
	if (AudioUnitSetProperty(outputUnit_, kAudioUnitProperty_StreamFormat,
				 kAudioUnitScope_Input, 0, &fmt, sizeof(fmt)) != noErr)
		return false;

	AURenderCallbackStruct cb{};
	cb.inputProc = renderThunk;
	cb.inputProcRefCon = this;
	if (AudioUnitSetProperty(outputUnit_, kAudioUnitProperty_SetRenderCallback,
				 kAudioUnitScope_Input, 0, &cb, sizeof(cb)) != noErr)
		return false;

	return AudioUnitInitialize(outputUnit_) == noErr;
}

void VirtualMicEngine::teardownAudioUnit()
{
	if (!outputUnit_)
		return;
	AudioUnitUninitialize(outputUnit_);
	AudioComponentInstanceDispose(outputUnit_);
	outputUnit_ = nullptr;
}

void VirtualMicEngine::obsAudioThunk(void *param, size_t, struct audio_data *data)
{
	static_cast<VirtualMicEngine *>(param)->onObsAudio(data);
}

void VirtualMicEngine::onObsAudio(struct audio_data *data)
{
	if (!data || !data->data[0] || data->frames == 0)
		return;

	const size_t frames = static_cast<size_t>(data->frames);
	const size_t totalSamples = frames * channels_;
	const float gain = muted_.load() ? 0.0f
					 : static_cast<float>(std::pow(10.0, gainDb_.load() / 20.0));

	auto *src = reinterpret_cast<const float *>(data->data[0]);

	// Stage 1: apply user gain/mute into a scratch buffer.
	static thread_local std::vector<float> scratch;
	scratch.resize(totalSamples);
	for (size_t i = 0; i < totalSamples; ++i)
		scratch[i] = src[i] * gain;

	// Stage 2: processing chain (NS -> compressor -> limiter), each gated.
	if (ns_ && nsEnabled_.load())
		ns_->process(scratch.data(), frames);
	if (compEnabled_.load())
		comp_.process(scratch.data(), frames, channels_);
	if (limiterEnabled_.load())
		limiter_.process(scratch.data(), frames, channels_);

	// Stage 3: track peak on the *processed* signal (what's actually sent).
	float localPeak = 0.0f;
	for (size_t i = 0; i < totalSamples; ++i) {
		float a = std::fabs(scratch[i]);
		if (a > localPeak)
			localPeak = a;
	}
	float prev = peak_.load();
	while (localPeak > prev && !peak_.compare_exchange_weak(prev, localPeak))
		;

	// Stage 4: to the virtual mic, and (optionally) tee to local monitoring.
	ring_.write(scratch.data(), totalSamples);

	if (monitorEnabled_.load()) {
		const float mGain =
			static_cast<float>(std::pow(10.0, monitorGainDb_.load() / 20.0));
		if (mGain != 1.0f) {
			static thread_local std::vector<float> mScratch;
			mScratch.resize(totalSamples);
			for (size_t i = 0; i < totalSamples; ++i)
				mScratch[i] = scratch[i] * mGain;
			monitorRing_.write(mScratch.data(), totalSamples);
		} else {
			monitorRing_.write(scratch.data(), totalSamples);
		}
	}
}

OSStatus VirtualMicEngine::renderThunk(void *inRefCon, AudioUnitRenderActionFlags *,
				       const AudioTimeStamp *, UInt32, UInt32 frames,
				       AudioBufferList *io)
{
	auto *self = static_cast<VirtualMicEngine *>(inRefCon);
	if (!io || io->mNumberBuffers == 0)
		return noErr;

	auto *out = static_cast<float *>(io->mBuffers[0].mData);
	const size_t need = static_cast<size_t>(frames) * self->channels_;
	self->ring_.readOrSilence(out, need);
	return noErr;
}

// ---- Local monitoring ------------------------------------------------------

bool VirtualMicEngine::setMonitor(bool on)
{
	if (!on) {
		monitorEnabled_.store(false);
		teardownMonitorUnit();
		monitorFeedback_.store(false);
		return true;
	}

	if (monitorEnabled_.load())
		return true;

	// Resolve the chosen monitor device (empty UID = follow system default).
	AudioDeviceID target = monitorUID_.empty()
				       ? AudioDeviceControl::getDefaultOutputDevice()
				       : AudioDeviceControl::findDeviceByUID(monitorUID_);

	// Refuse if the target is our own virtual device: routing the monitor
	// there would loop OBS Audio straight back into itself.
	std::string vname, vuid;
	AudioDeviceID vdev = AudioDeviceControl::findVirtualDevice(&vname, &vuid);
	if (target == kAudioObjectUnknown || target == vdev) {
		monitorFeedback_.store(target == vdev && target != kAudioObjectUnknown);
		ENGINE_LOG(LOG_WARNING,
			   "Monitor not enabled: target is missing or is the virtual "
			   "device (would feed back). Pick real speakers/headphones.");
		return false;
	}
	monitorFeedback_.store(false);
	monitorDeviceId_ = target;

	monitorRing_.clear();
	if (!setupMonitorUnit()) {
		ENGINE_LOG(LOG_ERROR, "Failed to set up monitor output unit");
		teardownMonitorUnit();
		return false;
	}
	if (AudioOutputUnitStart(monitorUnit_) != noErr) {
		ENGINE_LOG(LOG_ERROR, "AudioOutputUnitStart (monitor) failed");
		teardownMonitorUnit();
		return false;
	}
	monitorEnabled_.store(true);
	ENGINE_LOG(LOG_INFO, "Local monitoring on -> default output device");
	return true;
}

bool VirtualMicEngine::setupMonitorUnit()
{
	AudioComponentDescription desc{};
	desc.componentType = kAudioUnitType_Output;
	desc.componentSubType = kAudioUnitSubType_HALOutput;
	desc.componentManufacturer = kAudioUnitManufacturer_Apple;

	AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
	if (!comp)
		return false;
	if (AudioComponentInstanceNew(comp, &monitorUnit_) != noErr || !monitorUnit_)
		return false;

	UInt32 enable = 1, disable = 0;
	AudioUnitSetProperty(monitorUnit_, kAudioOutputUnitProperty_EnableIO,
			     kAudioUnitScope_Output, 0, &enable, sizeof(enable));
	AudioUnitSetProperty(monitorUnit_, kAudioOutputUnitProperty_EnableIO,
			     kAudioUnitScope_Input, 1, &disable, sizeof(disable));

	// Bind to the resolved monitor device (chosen in the dock, or default).
	AudioDeviceID dev = monitorDeviceId_ != kAudioObjectUnknown
				    ? monitorDeviceId_
				    : AudioDeviceControl::getDefaultOutputDevice();
	if (AudioUnitSetProperty(monitorUnit_, kAudioOutputUnitProperty_CurrentDevice,
				 kAudioUnitScope_Global, 0, &dev, sizeof(dev)) != noErr)
		return false;

	AudioStreamBasicDescription fmt{};
	fmt.mSampleRate = sampleRate_;
	fmt.mFormatID = kAudioFormatLinearPCM;
	fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
	fmt.mChannelsPerFrame = channels_;
	fmt.mBitsPerChannel = 32;
	fmt.mFramesPerPacket = 1;
	fmt.mBytesPerFrame = sizeof(float) * channels_;
	fmt.mBytesPerPacket = fmt.mBytesPerFrame;
	if (AudioUnitSetProperty(monitorUnit_, kAudioUnitProperty_StreamFormat,
				 kAudioUnitScope_Input, 0, &fmt, sizeof(fmt)) != noErr)
		return false;

	AURenderCallbackStruct cb{};
	cb.inputProc = monitorRenderThunk;
	cb.inputProcRefCon = this;
	if (AudioUnitSetProperty(monitorUnit_, kAudioUnitProperty_SetRenderCallback,
				 kAudioUnitScope_Input, 0, &cb, sizeof(cb)) != noErr)
		return false;

	return AudioUnitInitialize(monitorUnit_) == noErr;
}

void VirtualMicEngine::teardownMonitorUnit()
{
	if (!monitorUnit_)
		return;
	AudioOutputUnitStop(monitorUnit_);
	AudioUnitUninitialize(monitorUnit_);
	AudioComponentInstanceDispose(monitorUnit_);
	monitorUnit_ = nullptr;
}

OSStatus VirtualMicEngine::monitorRenderThunk(void *inRefCon, AudioUnitRenderActionFlags *,
					      const AudioTimeStamp *, UInt32, UInt32 frames,
					      AudioBufferList *io)
{
	auto *self = static_cast<VirtualMicEngine *>(inRefCon);
	if (!io || io->mNumberBuffers == 0)
		return noErr;
	auto *out = static_cast<float *>(io->mBuffers[0].mData);
	const size_t need = static_cast<size_t>(frames) * self->channels_;
	self->monitorRing_.readOrSilence(out, need);
	return noErr;
}

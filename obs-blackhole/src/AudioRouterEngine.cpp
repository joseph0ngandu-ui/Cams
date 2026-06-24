#include "AudioRouterEngine.h"
#include "AudioDeviceControl.h"

#include <util/base.h>

#include <cmath>

#define RLOG(level, fmt, ...) blog(level, "[obs-blackhole/router] " fmt, ##__VA_ARGS__)

AudioRouterEngine::~AudioRouterEngine()
{
	stop();
}

bool AudioRouterEngine::start(const std::string &inputUID, const std::string &outputUID)
{
	if (running_.load())
		return true;

	AudioDeviceID inDev = AudioDeviceControl::findDeviceByUID(inputUID);
	AudioDeviceID outDev = AudioDeviceControl::findDeviceByUID(outputUID);
	if (inDev == kAudioObjectUnknown || outDev == kAudioObjectUnknown) {
		RLOG(LOG_WARNING, "Pick a valid input and output device first.");
		return false;
	}
	if (inDev == outDev) {
		RLOG(LOG_WARNING, "Input and output are the same device — would feed back.");
		return false;
	}

	inputName_ = AudioDeviceControl::deviceName(inDev);
	outputName_ = AudioDeviceControl::deviceName(outDev);

	captureChannels_ = AudioDeviceControl::channelCount(inDev, true);
	if (captureChannels_ < 1)
		captureChannels_ = 1;
	if (captureChannels_ > 2)
		captureChannels_ = 2;

	// ~500 ms of slack to absorb the drift between the two device clocks.
	ring_.init(sampleRate_ / 2, kOutChannels);
	ring_.clear();

	// Prepare the processing chain (48 kHz internal so RNNoise can run).
	comp_.prepare(sampleRate_);
	limiter_.prepare(sampleRate_);
	ns_ = std::make_unique<NoiseSuppressor>();
	if (!ns_->prepare(captureChannels_)) {
		RLOG(LOG_WARNING, "RNNoise init failed; noise suppression disabled");
		ns_.reset();
	}

	if (!setupOutputUnit(outDev)) {
		RLOG(LOG_ERROR, "Failed to set up output unit");
		teardown();
		return false;
	}
	if (!setupInputUnit(inDev)) {
		RLOG(LOG_ERROR, "Failed to set up input (capture) unit");
		teardown();
		return false;
	}

	if (AudioOutputUnitStart(outputUnit_) != noErr ||
	    AudioOutputUnitStart(inputUnit_) != noErr) {
		RLOG(LOG_ERROR, "AudioOutputUnitStart failed");
		teardown();
		return false;
	}

	running_.store(true);
	RLOG(LOG_INFO, "Routing '%s' (%u ch) -> '%s'", inputName_.c_str(), captureChannels_,
	     outputName_.c_str());
	return true;
}

void AudioRouterEngine::stop()
{
	if (!running_.exchange(false))
		return;
	teardown();
	ns_.reset();
	RLOG(LOG_INFO, "Routing stopped");
}

void AudioRouterEngine::teardown()
{
	if (inputUnit_) {
		AudioOutputUnitStop(inputUnit_);
		AudioUnitUninitialize(inputUnit_);
		AudioComponentInstanceDispose(inputUnit_);
		inputUnit_ = nullptr;
	}
	if (outputUnit_) {
		AudioOutputUnitStop(outputUnit_);
		AudioUnitUninitialize(outputUnit_);
		AudioComponentInstanceDispose(outputUnit_);
		outputUnit_ = nullptr;
	}
}

// ---- Input (capture) unit --------------------------------------------------

bool AudioRouterEngine::setupInputUnit(AudioDeviceID device)
{
	AudioComponentDescription desc{};
	desc.componentType = kAudioUnitType_Output;
	desc.componentSubType = kAudioUnitSubType_HALOutput;
	desc.componentManufacturer = kAudioUnitManufacturer_Apple;

	AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
	if (!comp)
		return false;
	if (AudioComponentInstanceNew(comp, &inputUnit_) != noErr || !inputUnit_)
		return false;

	// Enable input (bus 1), disable output (bus 0).
	UInt32 enable = 1, disable = 0;
	if (AudioUnitSetProperty(inputUnit_, kAudioOutputUnitProperty_EnableIO,
				 kAudioUnitScope_Input, 1, &enable, sizeof(enable)) != noErr)
		return false;
	AudioUnitSetProperty(inputUnit_, kAudioOutputUnitProperty_EnableIO,
			     kAudioUnitScope_Output, 0, &disable, sizeof(disable));

	// Bind to the capture (mic) device.
	if (AudioUnitSetProperty(inputUnit_, kAudioOutputUnitProperty_CurrentDevice,
				 kAudioUnitScope_Global, 0, &device, sizeof(device)) != noErr)
		return false;

	// Client format we want to receive: interleaved Float32 at 48 kHz.
	AudioStreamBasicDescription fmt{};
	fmt.mSampleRate = sampleRate_;
	fmt.mFormatID = kAudioFormatLinearPCM;
	fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
	fmt.mChannelsPerFrame = captureChannels_;
	fmt.mBitsPerChannel = 32;
	fmt.mFramesPerPacket = 1;
	fmt.mBytesPerFrame = sizeof(float) * captureChannels_;
	fmt.mBytesPerPacket = fmt.mBytesPerFrame;
	if (AudioUnitSetProperty(inputUnit_, kAudioUnitProperty_StreamFormat,
				 kAudioUnitScope_Output, 1, &fmt, sizeof(fmt)) != noErr)
		return false;

	// Size our render scratch from the unit's maximum slice.
	UInt32 maxFrames = 4096;
	UInt32 sz = sizeof(maxFrames);
	AudioUnitGetProperty(inputUnit_, kAudioUnitProperty_MaximumFramesPerSlice,
			     kAudioUnitScope_Global, 0, &maxFrames, &sz);
	maxFrames_ = maxFrames ? maxFrames : 4096;
	captureBuf_.assign((size_t)maxFrames_ * captureChannels_, 0.0f);
	stereoBuf_.assign((size_t)maxFrames_ * kOutChannels, 0.0f);

	AURenderCallbackStruct cb{};
	cb.inputProc = inputThunk;
	cb.inputProcRefCon = this;
	if (AudioUnitSetProperty(inputUnit_, kAudioOutputUnitProperty_SetInputCallback,
				 kAudioUnitScope_Global, 0, &cb, sizeof(cb)) != noErr)
		return false;

	return AudioUnitInitialize(inputUnit_) == noErr;
}

OSStatus AudioRouterEngine::inputThunk(void *inRefCon, AudioUnitRenderActionFlags *flags,
				       const AudioTimeStamp *ts, UInt32 bus, UInt32 frames,
				       AudioBufferList *)
{
	static_cast<AudioRouterEngine *>(inRefCon)->onCapture(flags, ts, bus, frames);
	return noErr;
}

void AudioRouterEngine::onCapture(AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts,
				  UInt32 bus, UInt32 frames)
{
	if (frames == 0 || frames > maxFrames_)
		return;

	const uint32_t ch = captureChannels_;

	// Pull the captured audio into our interleaved scratch buffer.
	AudioBufferList abl;
	abl.mNumberBuffers = 1;
	abl.mBuffers[0].mNumberChannels = ch;
	abl.mBuffers[0].mDataByteSize = frames * ch * sizeof(float);
	abl.mBuffers[0].mData = captureBuf_.data();
	if (AudioUnitRender(inputUnit_, flags, ts, bus, frames, &abl) != noErr)
		return;

	const size_t capSamples = (size_t)frames * ch;
	const float gain = muted_.load()
				   ? 0.0f
				   : static_cast<float>(std::pow(10.0, gainDb_.load() / 20.0));
	for (size_t i = 0; i < capSamples; ++i)
		captureBuf_[i] *= gain;

	// Processing chain on the native channel count.
	if (ns_ && nsEnabled_.load())
		ns_->process(captureBuf_.data(), frames);
	if (compEnabled_.load())
		comp_.process(captureBuf_.data(), frames, ch);
	if (limiterEnabled_.load())
		limiter_.process(captureBuf_.data(), frames, ch);

	// Peak on the processed signal.
	float localPeak = 0.0f;
	for (size_t i = 0; i < capSamples; ++i) {
		float a = std::fabs(captureBuf_[i]);
		if (a > localPeak)
			localPeak = a;
	}
	float prev = peak_.load();
	while (localPeak > prev && !peak_.compare_exchange_weak(prev, localPeak))
		;

	// Expand to stereo for the output device, then hand off to the ring.
	for (UInt32 i = 0; i < frames; ++i) {
		float l = captureBuf_[i * ch];
		float r = (ch >= 2) ? captureBuf_[i * ch + 1] : l;
		stereoBuf_[i * kOutChannels] = l;
		stereoBuf_[i * kOutChannels + 1] = r;
	}
	ring_.write(stereoBuf_.data(), (size_t)frames * kOutChannels);
}

// ---- Output unit (same pattern as VirtualMicEngine) ------------------------

bool AudioRouterEngine::setupOutputUnit(AudioDeviceID device)
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

	UInt32 enable = 1, disable = 0;
	AudioUnitSetProperty(outputUnit_, kAudioOutputUnitProperty_EnableIO,
			     kAudioUnitScope_Output, 0, &enable, sizeof(enable));
	AudioUnitSetProperty(outputUnit_, kAudioOutputUnitProperty_EnableIO,
			     kAudioUnitScope_Input, 1, &disable, sizeof(disable));

	if (AudioUnitSetProperty(outputUnit_, kAudioOutputUnitProperty_CurrentDevice,
				 kAudioUnitScope_Global, 0, &device, sizeof(device)) != noErr)
		return false;

	AudioStreamBasicDescription fmt{};
	fmt.mSampleRate = sampleRate_;
	fmt.mFormatID = kAudioFormatLinearPCM;
	fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
	fmt.mChannelsPerFrame = kOutChannels;
	fmt.mBitsPerChannel = 32;
	fmt.mFramesPerPacket = 1;
	fmt.mBytesPerFrame = sizeof(float) * kOutChannels;
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

OSStatus AudioRouterEngine::renderThunk(void *inRefCon, AudioUnitRenderActionFlags *,
					const AudioTimeStamp *, UInt32, UInt32 frames,
					AudioBufferList *io)
{
	auto *self = static_cast<AudioRouterEngine *>(inRefCon);
	if (!io || io->mNumberBuffers == 0)
		return noErr;
	auto *out = static_cast<float *>(io->mBuffers[0].mData);
	self->ring_.readOrSilence(out, (size_t)frames * kOutChannels);
	return noErr;
}

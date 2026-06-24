#include "AudioDeviceControl.h"

#include <CoreFoundation/CoreFoundation.h>
#include <sys/stat.h>
#include <algorithm>

namespace {

std::string cfStringToStd(CFStringRef s)
{
	if (!s)
		return {};
	CFIndex len = CFStringGetLength(s);
	CFIndex max = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
	std::string out(static_cast<size_t>(max), '\0');
	if (CFStringGetCString(s, out.data(), max, kCFStringEncodingUTF8))
		out.resize(strlen(out.c_str()));
	else
		out.clear();
	return out;
}

bool getStringProp(AudioObjectID obj, AudioObjectPropertySelector sel, std::string &out)
{
	AudioObjectPropertyAddress addr{sel, kAudioObjectPropertyScopeGlobal,
					kAudioObjectPropertyElementMain};
	CFStringRef str = nullptr;
	UInt32 size = sizeof(str);
	if (AudioObjectGetPropertyData(obj, &addr, 0, nullptr, &size, &str) != noErr || !str)
		return false;
	out = cfStringToStd(str);
	CFRelease(str);
	return true;
}

uint32_t channelsForScope(AudioDeviceID id, AudioObjectPropertyScope scope)
{
	AudioObjectPropertyAddress addr{kAudioDevicePropertyStreamConfiguration, scope,
					kAudioObjectPropertyElementMain};
	UInt32 size = 0;
	if (AudioObjectGetPropertyDataSize(id, &addr, 0, nullptr, &size) != noErr || size == 0)
		return 0;
	std::vector<uint8_t> raw(size);
	auto *bl = reinterpret_cast<AudioBufferList *>(raw.data());
	if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, bl) != noErr)
		return 0;
	uint32_t ch = 0;
	for (UInt32 i = 0; i < bl->mNumberBuffers; ++i)
		ch += bl->mBuffers[i].mNumberChannels;
	return ch;
}

bool pathExists(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0;
}

} // namespace

namespace AudioDeviceControl {

std::vector<AudioDeviceInfo> listDevices()
{
	std::vector<AudioDeviceInfo> result;
	AudioObjectPropertyAddress addr{kAudioHardwarePropertyDevices,
					kAudioObjectPropertyScopeGlobal,
					kAudioObjectPropertyElementMain};
	UInt32 size = 0;
	if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size) !=
	    noErr)
		return result;
	const size_t count = size / sizeof(AudioDeviceID);
	std::vector<AudioDeviceID> ids(count);
	if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size,
				       ids.data()) != noErr)
		return result;

	for (AudioDeviceID id : ids) {
		AudioDeviceInfo info;
		info.id = id;
		getStringProp(id, kAudioDevicePropertyDeviceNameCFString, info.name);
		getStringProp(id, kAudioDevicePropertyDeviceUID, info.uid);
		info.inputChannels = channelsForScope(id, kAudioDevicePropertyScopeInput);
		info.outputChannels = channelsForScope(id, kAudioDevicePropertyScopeOutput);
		info.sampleRate = getNominalSampleRate(id);
		result.push_back(std::move(info));
	}
	return result;
}

AudioDeviceID findVirtualDevice(std::string *nameOut, std::string *uidOut)
{
	auto devices = listDevices();
	auto pick = [&](const AudioDeviceInfo &d) {
		if (nameOut)
			*nameOut = d.name;
		if (uidOut)
			*uidOut = d.uid;
		return d.id;
	};
	// 1) exact fork UID
	for (auto &d : devices)
		if (d.uid == "OBSAudio_UID")
			return pick(d);
	// 2) name contains "OBS Audio"
	for (auto &d : devices)
		if (d.name.find("OBS Audio") != std::string::npos)
			return pick(d);
	// 3) stock BlackHole fallback
	for (auto &d : devices)
		if (d.name.find("BlackHole") != std::string::npos)
			return pick(d);
	return kAudioObjectUnknown;
}

AudioDeviceID findDeviceByUID(const std::string &uid)
{
	if (uid.empty())
		return kAudioObjectUnknown;
	for (auto &d : listDevices())
		if (d.uid == uid)
			return d.id;
	return kAudioObjectUnknown;
}

bool driverInstalledOnDisk(std::string *whichOut)
{
	const char *hal = "/Library/Audio/Plug-Ins/HAL/";
	struct {
		const char *path;
		const char *label;
	} candidates[] = {
		{"/Library/Audio/Plug-Ins/HAL/OBS Audio.driver", "OBS Audio (fork)"},
		{"/Library/Audio/Plug-Ins/HAL/BlackHole2ch.driver", "BlackHole 2ch (stock)"},
		{"/Library/Audio/Plug-Ins/HAL/BlackHole16ch.driver", "BlackHole 16ch (stock)"},
	};
	(void)hal;
	for (auto &c : candidates) {
		if (pathExists(c.path)) {
			if (whichOut)
				*whichOut = c.label;
			return true;
		}
	}
	return false;
}

std::string deviceName(AudioDeviceID id)
{
	std::string s;
	getStringProp(id, kAudioDevicePropertyDeviceNameCFString, s);
	return s;
}

std::string deviceUID(AudioDeviceID id)
{
	std::string s;
	getStringProp(id, kAudioDevicePropertyDeviceUID, s);
	return s;
}

double getNominalSampleRate(AudioDeviceID id)
{
	AudioObjectPropertyAddress addr{kAudioDevicePropertyNominalSampleRate,
					kAudioObjectPropertyScopeGlobal,
					kAudioObjectPropertyElementMain};
	Float64 rate = 0.0;
	UInt32 size = sizeof(rate);
	if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, &rate) != noErr)
		return 0.0;
	return rate;
}

bool setNominalSampleRate(AudioDeviceID id, double rate)
{
	AudioObjectPropertyAddress addr{kAudioDevicePropertyNominalSampleRate,
					kAudioObjectPropertyScopeGlobal,
					kAudioObjectPropertyElementMain};
	Float64 r = rate;
	return AudioObjectSetPropertyData(id, &addr, 0, nullptr, sizeof(r), &r) == noErr;
}

std::vector<double> availableSampleRates(AudioDeviceID id)
{
	std::vector<double> rates;
	AudioObjectPropertyAddress addr{kAudioDevicePropertyAvailableNominalSampleRates,
					kAudioObjectPropertyScopeGlobal,
					kAudioObjectPropertyElementMain};
	UInt32 size = 0;
	if (AudioObjectGetPropertyDataSize(id, &addr, 0, nullptr, &size) != noErr || size == 0)
		return rates;
	std::vector<AudioValueRange> ranges(size / sizeof(AudioValueRange));
	if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, ranges.data()) != noErr)
		return rates;
	for (auto &r : ranges) {
		// Discrete rates report min == max.
		if (r.mMinimum == r.mMaximum)
			rates.push_back(r.mMinimum);
		else
			rates.push_back(r.mMaximum);
	}
	std::sort(rates.begin(), rates.end());
	rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
	return rates;
}

uint32_t channelCount(AudioDeviceID id, bool input)
{
	return channelsForScope(id, input ? kAudioDevicePropertyScopeInput
					  : kAudioDevicePropertyScopeOutput);
}

AudioDeviceID getDefaultOutputDevice()
{
	AudioObjectPropertyAddress addr{kAudioHardwarePropertyDefaultOutputDevice,
					kAudioObjectPropertyScopeGlobal,
					kAudioObjectPropertyElementMain};
	AudioDeviceID id = kAudioObjectUnknown;
	UInt32 size = sizeof(id);
	AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, &id);
	return id;
}

bool setDefaultOutputDevice(AudioDeviceID id)
{
	AudioObjectPropertyAddress addr{kAudioHardwarePropertyDefaultOutputDevice,
					kAudioObjectPropertyScopeGlobal,
					kAudioObjectPropertyElementMain};
	return AudioObjectSetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr,
					  sizeof(id), &id) == noErr;
}

} // namespace AudioDeviceControl

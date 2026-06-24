/*
 * AudioDeviceControl.h — thin CoreAudio HAL wrapper for discovering and
 * controlling the virtual audio device, so the OBS dock can do everything a
 * user would otherwise open Audio MIDI Setup for.
 */
#pragma once

#include <CoreAudio/CoreAudio.h>
#include <string>
#include <vector>

struct AudioDeviceInfo {
	AudioDeviceID id = kAudioObjectUnknown;
	std::string name;
	std::string uid;
	uint32_t inputChannels = 0;
	uint32_t outputChannels = 0;
	double sampleRate = 0.0;
};

namespace AudioDeviceControl {

// Enumerate all CoreAudio devices on the system.
std::vector<AudioDeviceInfo> listDevices();

// Locate our virtual device. Preference order:
//   1) exact UID "OBSAudio_UID" (the OBS-branded fork)
//   2) name contains "OBS Audio"
//   3) name contains "BlackHole" (stock driver, so the plugin works pre-fork)
// Returns kAudioObjectUnknown if none found.
AudioDeviceID findVirtualDevice(std::string *nameOut = nullptr,
				std::string *uidOut = nullptr);

// Resolve a stored device UID back to a live device ID (stable across
// reconnects). Returns kAudioObjectUnknown if no device currently has that UID.
AudioDeviceID findDeviceByUID(const std::string &uid);

// True if a HAL .driver bundle for our fork OR stock BlackHole is installed
// on disk (it may be installed but not yet live until coreaudiod reloads).
bool driverInstalledOnDisk(std::string *whichOut = nullptr);

std::string deviceName(AudioDeviceID id);
std::string deviceUID(AudioDeviceID id);
double getNominalSampleRate(AudioDeviceID id);
bool setNominalSampleRate(AudioDeviceID id, double rate);
std::vector<double> availableSampleRates(AudioDeviceID id);
uint32_t channelCount(AudioDeviceID id, bool input);

// Default *output* device control (so all system audio can be routed in).
AudioDeviceID getDefaultOutputDevice();
bool setDefaultOutputDevice(AudioDeviceID id);

} // namespace AudioDeviceControl

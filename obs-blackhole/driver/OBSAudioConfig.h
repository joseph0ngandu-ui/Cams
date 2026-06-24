/*
 * OBSAudioConfig.h — OBS-native rebrand of the BlackHole virtual audio driver.
 *
 * This header is force-included (clang -include) when compiling BlackHole.c so
 * that every customizable constant is defined *before* BlackHole's own
 * `#ifndef` guards run. Nothing in the upstream source is edited — we only
 * override, which keeps the fork trivial to rebase onto new BlackHole releases.
 *
 * The result is a driver whose device shows up everywhere (mic pickers,
 * Audio MIDI Setup, Zoom/Meet/Discord) simply as "OBS Audio".
 */

#ifndef OBS_AUDIO_CONFIG_H
#define OBS_AUDIO_CONFIG_H

/* Internal short name — used to derive stable CoreAudio UIDs (no spaces). */
#define kDriver_Name        "OBSAudio"

/* Must match PRODUCT_BUNDLE_IDENTIFIER passed to xcodebuild. */
#define kPlugIn_BundleID    "com.obsproject.obs-audio-driver"

/* Drop BlackHole's "%ich" channel-count suffix so the name is exactly ours.
 * With the format off, the device UID becomes "OBSAudio_UID" (stable). */
#define kHas_Driver_Name_Format 0

/* What the user actually sees in every app's audio device list. */
#define kDevice_Name        "OBS Audio"
#define kDevice2_Name       "OBS Audio (Aux)"

/* Branding. */
#define kManufacturer_Name  "OBS Project"

/* Stereo is all a "virtual microphone" needs. */
#define kNumber_Of_Channels 2

#endif /* OBS_AUDIO_CONFIG_H */

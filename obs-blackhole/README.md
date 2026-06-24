# obs-blackhole — native virtual audio for OBS (macOS)

Turns OBS's audio into a system **microphone** that any app (Zoom, Meet,
Discord, browsers, streaming services) can select as **"OBS Audio"**, plus an
in-app **BlackHole dock** so you never need to open Audio MIDI Setup or a
separate driver app.

Two pieces, both forked/tweaked from open source:

| Piece | What it is | Source |
|-------|-----------|--------|
| **`driver/`** | A rebrand of the [BlackHole](https://github.com/ExistentialAudio/BlackHole) virtual audio driver. Overrides (via `OBSAudioConfig.h`, no source edits) make the device appear everywhere as **"OBS Audio"** (UID `OBSAudio_UID`, manufacturer "OBS Project"). | BlackHole 0.6.1 |
| **`src/`** | An OBS Studio plugin: a Qt **dock** + a CoreAudio engine that taps OBS's final audio mix (`obs_add_raw_audio_callback`) and streams it into the virtual device. | This repo |

## The dock

- **Status** — live device name + state (Live / Ready / needs coreaudiod reload).
- **Virtual Microphone** — Start/Stop, gain (dB), mute, live output level meter, auto-start on launch. (Uses OBS's main mix.)
- **Processing** — a real DSP chain applied to OBS's mix before it reaches the virtual mic:
  - **Noise suppression** — RNNoise (the same neural denoiser OBS ships), compiled in. Removes hum/fans/keyboard/room noise. Requires OBS at 48 kHz (the checkbox greys out otherwise).
  - **Compressor** — evens out loud/quiet for a consistent voice level (broadcast-style defaults).
  - **Limiter** — brick-wall at ~-1 dBFS so the mic never digitally clips.
  - **Monitor on this Mac** — hear the processed mic through a **device you choose** (AirPods, any
    speaker, or "System default output"), with a level slider. Refuses to enable if the chosen target
    *is* "OBS Audio" (which would feed back).
- **Routing (mic → device)** — a VoiceMeeter-style point-to-point router, independent of OBS scenes:
  pick a **mic input device**, run it through the same noise-suppression/compressor/limiter chain, and
  send it **out to any output device** (e.g. a USB mixer's playback endpoint). Start/Stop, gain, mute,
  level meter. Refuses if input and output are the same device (feedback). Captures via a CoreAudio
  AUHAL input unit; the two device clocks are bridged by the ring (tolerates small drift).

Chain order (both paths): `gain/mute → noise suppression → compressor → limiter → output`.
There's also a global, rebindable **"Toggle OBS Virtual Mic"** hotkey (Settings → Hotkeys) and
auto-recover if the device drops out (e.g. a coreaudiod reload). All settings persist (devices by UID).

The plugin matches the device by UID `OBSAudio_UID`, then any name containing
"OBS Audio", then any "BlackHole" device — so it works with stock BlackHole too.

## Build

Prereqs (all vendored under `../.deps` by default): Qt 6.8.3 (matches OBS's
Qt), the libobs SDK, OBS source (for the frontend-api header), and Xcode.

```bash
# Plugin
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build          # -> ~/Library/Application Support/obs-studio/plugins

# Driver (rebranded BlackHole)
cd driver && ./build-driver.sh --install   # sudo-installs + reloads coreaudiod
```

## How it works end-to-end

```
OBS scene/sources
      │  (final mixed audio, per track)
      ▼
obs_add_raw_audio_callback  ──►  lock-free ring  ──►  CoreAudio HAL output unit
                                                              │
                                                              ▼
                                                   "OBS Audio" device
                                                              │  (loopback: output → input)
                                                              ▼
                                          shows up as a MICROPHONE in every app
```

macOS-only (CoreAudio). Requires a CoreAudio reload (reboot or
`sudo killall coreaudiod`) after the driver is first installed.

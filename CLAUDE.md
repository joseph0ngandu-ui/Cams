# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Common commands

- `cd CamsApp && swift build -c release` — compile the iOS app via Swift Package Manager (the same package supports SwiftUI entry point, encoder, and Bonjour/control helpers).
- `cd CamsApp && swift test` — run the `CamsCoreTests` suite that exercises the packet serialization and related utilities used by the streaming stack.
- `cd obs-plugin && cmake -B build -DOBS_SDK_ROOT=<path> -DFFMPEG_ROOT=<path> && cmake --build build --parallel` — configure and build the OBS plugin; replace the SDK/root flags per OS (mac: `-DFFMPEG_ROOT=/opt/homebrew`, linux: only OBS path, windows: add `-A x64` and Windows paths).
- `cd obs-plugin && cmake --install build` — install the plugin into the default OBS plugin directory for the host OS after a Release build.
- `cd obs-plugin && cmake -B build -DBUILD_PLUGIN=OFF -DBUILD_TESTS=ON && cmake --build build && ctest --test-dir build --output-on-failure` — build and run the standalone jitter-buffer unit tests without OBS/FFmpeg.

Refer to `README.md` for platform-specific build flag suggestions and the release checklist under `docs/RELEASE_CHECKLIST.md`.

## Architecture at a glance

- **CamsApp (Swift + SwiftUI)** — captures camera frames via `AVCaptureDevice`, passes zero-copy `CVPixelBuffer`s through a pre-allocated `CircularBufferPool`, encodes via `VTCompressionSession` (HEVC primary with H.264 fallback, CBR, thermal throttling), and ships packets over `NetworkTransport` (NWConnection UDP) to port 8888. `BonjourPublisher` advertises `_cams-video._udp`, while `ControlChannel` listens on port 8889 so OBS can request focus/exposure locks and keyframes.
- **Cams protocol** — every UDP packet begins with a fixed 17-byte header (`Sequence`, `Timestamp`, `Frame Type`, `Payload Length`) followed by NAL payloads; controls/commands share the same packet definitions so the OBS plugin can mirror the Swift-side structures.
- **OBS plugin (C++17 + CMake)** — `NetworkListener` opens a POSIX UDP socket to receive and validate Cams packets from port 8888, `JitterBuffer` reorders up to three frames while maintaining low latency, and `VideoDecoder` (FFmpeg + hardware accel) produces frames fed into `obs_source_output_video`. `ControlServer` mirrors the iOS control channel on UDP port 8889, and Bonjour browsing keeps the source list in sync with advertised devices.

Keep the README architecture diagram and the network port table in mind when linking Swift and C++ components; the repo is structured as two separate artifacts (`CamsApp` and `obs-plugin`) with their own toolchains and test suites.

# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Common commands

- `cd CamsApp && swift test` — run the `CamsCoreTests` suite. Note: `swift test` only covers `CamsProtocol.swift` and `NetworkTransport.swift` (the `CamsCore` SwiftPM target); capture, encoder, Bonjour, and UI code are iOS-framework-dependent and excluded from the package.
- `cd CamsApp && xcodegen generate && xcodebuild -project Cams.xcodeproj -scheme Cams -configuration Debug -destination 'generic/platform=iOS' build CODE_SIGNING_ALLOWED=NO` — generate and build the actual iOS app target. `project.yml` is the XcodeGen source of truth; `.xcodeproj` is gitignored and must be regenerated before building.
- `cd obs-plugin && cmake -B build -DOBS_SDK_ROOT=<path> -DFFMPEG_ROOT=<path> && cmake --build build --parallel` — configure and build the OBS plugin; replace the SDK/root flags per OS (mac: `-DFFMPEG_ROOT=/opt/homebrew`, linux: only OBS path, windows: add `-A x64` and Windows paths).
- `cd obs-plugin && cmake --install build` — install the plugin into the default OBS plugin directory for the host OS after a Release build.
- `cd obs-plugin && cmake -S . -B build -DBUILD_PLUGIN=OFF -DBUILD_TESTS=ON && cmake --build build && ctest --test-dir build --output-on-failure` — build and run the standalone core tests without OBS/FFmpeg.

Refer to `README.md` for platform-specific build flag suggestions and the release checklist under `docs/RELEASE_CHECKLIST.md`.

## Architecture at a glance

- **CamsApp (Swift + SwiftUI)** — captures camera frames via `AVCaptureDevice`, sends `CVPixelBuffer`s directly into `VTCompressionSession` (HEVC primary with H.264 fallback, CBR, thermal throttling), and ships whole encoded frames as one or more UDP fragments over `NetworkTransport` to port 8888. `BonjourPublisher` advertises `_cams-video._udp`, while `ControlChannel` listens on port 8889 so OBS can request focus/exposure locks and keyframes.
- **Cams protocol** — every UDP packet begins with a fixed 21-byte header (`Sequence`, `Timestamp`, `Frame Type`, `Fragment Index`, `Fragment Count`, `Payload Length`) followed by NAL payload bytes; all multi-byte fields are big-endian; all fragments for one encoded frame share a sequence number.
- **OBS plugin (C++17 + CMake)** — `NetworkListener` opens a POSIX UDP socket to receive and validate Cams packets from port 8888, `FrameReassembler` reconstructs fragmented encoded frames, `JitterBuffer` reorders complete frames, and `VideoDecoder` (FFmpeg + hardware accel) produces frames fed into `obs_source_output_video`. `ControlServer` mirrors the iOS control channel on UDP port 8889, and Bonjour browsing keeps the source list in sync with advertised devices.

## Critical cross-cutting concern

`CamsProtocol.swift` (Swift) and `CamsPacket.h` (C++) are the two halves of the wire format definition — they must be kept in sync. Any change to the packet header layout, frame type enum values, or control command values requires updating **both** files. The header is 21 bytes (`kHeaderSize = 21`); the fragment validation limits are also mirrored in both files.

Keep the README architecture diagram and the network port table in mind when linking Swift and C++ components; the repo is structured as two separate artifacts (`CamsApp` and `obs-plugin`) with their own toolchains and test suites.

# Cams

**Video-only iOS-to-OBS camera streaming over a custom UDP protocol.**

Cams turns an iPhone or iPad into a high-quality video source for [OBS Studio](https://obsproject.com). It streams HEVC (H.265) or H.264 video directly from the iOS camera to OBS over Wi-Fi using a custom UDP transport rather than RTSP or HTTP.

> Status: video-only v1. Audio transport is reserved for a future protocol revision, and latency claims should be based on measured release-checklist results for the tested device/network.

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Features](#features)
3. [Repository Structure](#repository-structure)
4. [iOS App — CamsApp](#ios-app--camsapp)
   - [Requirements](#requirements)
   - [Building](#building)
   - [Protocol Specification](#protocol-specification)
5. [OBS Plugin — obs-cams](#obs-plugin--obs-cams)
   - [Requirements](#requirements-1)
   - [Building](#building-1)
   - [Settings UI](#settings-ui)
6. [Network Ports](#network-ports)
7. [Performance Notes](#performance-notes)
8. [Contributing](#contributing)
9. [License](#license)

---

## Architecture Overview

```
┌───────────────────────────────────────────────────────────────────────────┐
│  iPhone / iPad (iOS 16+)                                                   │
│                                                                            │
│  AVCaptureDevice                                                           │
│      │ CVPixelBuffer (YUV 4:2:0)                                           │
│      ▼                                                                     │
│  VTCompressionSession ── HEVC Main / H.264 High, CBR ≤ 50 Mbps, Real-time │
│      │ CMSampleBuffer                                                      │
│      ▼                                                                     │
│  NetworkTransport (NWConnection / UDP)                                     │
│      │  Cams Packet: [Seq(4)|TS(8)|FT(1)|Frag(4)|Len(4)|NAL Payload]      │
│      │  Backpressure: drop whole encoded frames when queue is full         │
│      ▼                                                                     │
│  Wi-Fi UDP → port 8888                                                     │
│                                                                            │
│  BonjourPublisher: _cams-video._udp (zero-config discovery)                │
│  ControlChannel:  port 8889 (focus / exposure / keyframe)                 │
└───────────────────────────────────────────────────────────────────────────┘
                          ↕  UDP
┌───────────────────────────────────────────────────────────────────────────┐
│  OBS Studio Plugin (obs-cams)                                              │
│                                                                            │
│  NetworkListener (POSIX UDP socket)                                        │
│      │ Validated Cams packets                                              │
│      ▼                                                                     │
│  JitterBuffer (2–3 frame FIFO, sequence-number reordering)                 │
│      │ DecodedFrame                                                         │
│      ▼                                                                     │
│  VideoDecoder (FFmpeg libavcodec + hardware acceleration)                  │
│      │ AVFrame → video_data                                                │
│      ▼                                                                     │
│  obs_source_output_video()                                                 │
│                                                                            │
│  Bonjour browser: _cams-video._udp  (device discovery)                    │
│  ControlServer:   port 8889 (send focus/exposure/keyframe commands)        │
└───────────────────────────────────────────────────────────────────────────┘
```

---

## Features

| Feature | Detail |
|---|---|
| **Transport** | Custom UDP; no RTSP / HTTP overhead |
| **Codec** | HEVC (H.265) Main; automatic H.264 High fallback |
| **Bitrate** | CBR; configurable up to 50 Mbps for 4K |
| **Latency** | `kVTCompressionPropertyKey_RealTime = true`, `MaxFrameDelayCount = 0` |
| **Capture path** | `AVCaptureVideoDataOutput` CVPixelBuffers are submitted directly to VideoToolbox |
| **Congestion control** | Monitors `NWConnection` send-queue depth; drops whole encoded frames on backpressure |
| **Discovery** | Bonjour (`_cams-video._udp`) — zero configuration |
| **Jitter buffer** | Fixed-latency 0-frame (low latency) or 3-frame (stable) mode |
| **HW decode** | VideoToolbox (macOS), D3D11VA / CUDA (Windows), VAAPI / CUDA (Linux) |
| **Control** | UDP back-channel: focus lock, exposure lock, keyframe request, token-based ping/pong |
| **Thermal** | Encoder bitrate automatically halved / quartered under thermal pressure |

---

## Repository Structure

```
Cams/
├── CamsApp/                     # iOS application (Swift)
│   ├── Package.swift            # SwiftPM protocol/transport unit-test manifest
│   ├── project.yml              # XcodeGen project source of truth
│   ├── Sources/CamsApp/
│   │   ├── CamsProtocol.swift   # Packet header + control command definitions
│   │   ├── CircularBufferPool.swift  # Reserved helper; not active in video v1 capture path
│   │   ├── VideoEncoder.swift   # VideoToolbox encoder (HEVC / H.264, CBR)
│   │   ├── NetworkTransport.swift    # NWConnection UDP sender + backpressure
│   │   ├── BonjourPublisher.swift    # NWListener Bonjour advertisement
│   │   ├── CaptureSession.swift      # AVCaptureVideoDataOutput pipeline
│   │   ├── ControlChannel.swift      # UDP back-channel receiver (port 8889)
│   │   ├── ContentView.swift         # SwiftUI streaming UI
│   │   └── CamsApp.swift             # App entry point
│   └── Tests/CamsCoreTests/
│       └── CamsProtocolTests.swift   # Unit tests for packet serialisation
│
├── obs-plugin/                  # OBS Studio plugin (C++17)
│   ├── CMakeLists.txt           # Complete build system
│   ├── src/
│   │   ├── CamsPacket.h         # Packet definitions (mirrors CamsProtocol.swift)
│   │   ├── FrameReassembler.h/.cpp # UDP fragment reassembly
│   │   ├── JitterBuffer.h/.cpp  # Fixed-latency frame reordering buffer
│   │   ├── NetworkListener.h/.cpp   # UDP socket + Bonjour browser
│   │   ├── VideoDecoder.h/.cpp  # FFmpeg hardware-accelerated decoder
│   │   ├── ControlServer.h/.cpp # UDP control back-channel server
│   │   ├── CamsSource.h/.cpp    # obs_source_info implementation
│   │   └── plugin-main.cpp      # Plugin entry point
│   └── tests/
│       └── JitterBufferTest.cpp # Standalone jitter-buffer unit tests
│
├── .gitignore
└── README.md
```

---

## iOS App — CamsApp

### Requirements

- Xcode 15+
- iOS 16.0+ deployment target
- iPhone or iPad with a rear camera (4K recommended)
- Swift 5.9+

### Building

**Run Swift unit tests (protocol + packetisation core):**

```bash
cd CamsApp
swift test
```

**Build the iOS app from the generated Xcode project:**

```bash
cd CamsApp
xcodegen generate
xcodebuild -project Cams.xcodeproj \
    -scheme Cams \
    -configuration Debug \
    -destination 'generic/platform=iOS' \
    build CODE_SIGNING_ALLOWED=NO
```

`project.yml` is the tracked source of truth. Generated `.xcodeproj` files are ignored.

> **Note:** The app requires camera and local network permissions. These must be accepted at runtime.

### Protocol Specification

Every UDP datagram sent from the iOS app to OBS begins with a 21-byte header:

```
 0               1               2               3
 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
├───────────────────────────────────────────────────────────────────┤
│                     Sequence Number (32-bit)                       │  bytes 0–3
├───────────────────────────────────────────────────────────────────┤
│                                                                   │
│                      Timestamp (64-bit, ns)                       │  bytes 4–11
│                                                                   │
├───────────────────────────────────────────────────────────────────┤
│  Frame Type (8-bit)  │ Fragment Index (16-bit)                    │  bytes 12–14
├──────────────────────┴────────────────────────────────────────────┤
│ Fragment Count (16-bit)   │ Payload Length (32-bit)               │  bytes 15–20
├───────────────────────────────────────────────────────────────────┤
│                      Payload (variable)                           │
└───────────────────────────────────────────────────────────────────┘
```

All multi-byte fields are **big-endian** (network byte order).

| Frame Type | Value | Description |
|---|---|---|
| H264 | `0x01` | H.264 NAL unit(s) |
| HEVC | `0x02` | HEVC NAL unit(s) |
| ParameterSets | `0x03` | SPS / PPS / VPS |
| Keyframe | `0x04` | IDR frame |
| AudioPCM | `0x10` | Reserved; not emitted in video-only v1 |
| EndOfStream | `0xFF` | Session termination |

`Sequence Number` identifies the encoded frame. All fragments for the same encoded frame share the same sequence number, timestamp, and frame type.

---

## OBS Plugin — obs-cams

### Requirements

- CMake 3.20+
- OBS Studio SDK (libobs headers + library)
- FFmpeg 4.x+ (`libavcodec`, `libavutil`, `libavformat`)
- C++17-capable compiler:
  - macOS: Apple Clang 14+ / Xcode 14+
  - Linux: GCC 10+ or Clang 12+
  - Windows: MSVC 2019+ or Clang-CL

**Optional (for Bonjour discovery):**
- macOS: built-in (dns_sd / mDNSResponder)
- Linux: `avahi-compat-libdns_sd` (`apt install libavahi-compat-libdnssd-dev`)
- Windows: Apple Bonjour SDK for Windows

### Building

```bash
cd obs-plugin

# macOS (universal binary — arm64 + x86_64)
cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DOBS_SDK_ROOT=/path/to/obs-studio \
    -DFFMPEG_ROOT=/opt/homebrew
cmake --build build --parallel

# Linux
cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DOBS_SDK_ROOT=/usr/lib/obs-studio
cmake --build build --parallel

# Windows (x64, MSVC)
cmake -B build -A x64 \
    -DCMAKE_BUILD_TYPE=Release \
    -DOBS_SDK_ROOT="C:/obs-studio" \
    -DFFMPEG_ROOT="C:/ffmpeg"
cmake --build build --config Release --parallel
```

**Install the plugin:**

```bash
cmake --install build
```

The default install path is:
- macOS: `~/Library/Application Support/obs-studio/plugins/obs-cams/bin/`
- Linux: `~/.config/obs-studio/plugins/obs-cams/bin/64bit/`
- Windows: `C:\Program Files\obs-studio\obs-plugins\64bit\`

**Build the standalone unit tests only (no OBS or FFmpeg required):**

```bash
cmake -B build -DBUILD_PLUGIN=OFF -DBUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

### Settings UI

Once the plugin is installed and OBS is restarted, add a **Cams iOS Camera** source:

| Setting | Description |
|---|---|
| **iOS Device** | Dropdown populated with Bonjour-discovered Cams devices on the local network |
| **Refresh Devices** | Re-scans for Bonjour services |
| **Buffer Mode** | *Low Latency* (0-frame pass-through) or *Stable* (3-frame reordering jitter buffer) |
| **Video Quality** | Requests Low (720p30), Medium (1080p30), or High (4K60) from the iOS app |
| **Toggle Focus Lock** | Sends a `lockFocus` / `unlockFocus` command to the iOS device |
| **Toggle Exposure Lock** | Sends a `lockExposure` / `unlockExposure` command to the iOS device |

---

## Network Ports

| Port | Protocol | Direction | Purpose |
|---|---|---|---|
| **8888** | UDP | iOS → OBS | Video stream |
| **8889** | UDP | OBS ↔ iOS | Control back-channel (focus, exposure, keyframe) |

Ensure your firewall and Wi-Fi router allow UDP traffic on both ports within your local network.

---

## Performance Notes

- **ARM64 (Apple Silicon):** The CMakeLists.txt compiles a universal binary (`arm64;x86_64`) on macOS. VideoToolbox hardware encode/decode runs natively on the ANE/Media Engine.
- **Direct capture path:** `CVPixelBuffer` objects from `AVCaptureVideoDataOutput` are submitted directly to `VTCompressionSession` without intermediate copies.
- **Backpressure:** If the OBS host cannot consume frames fast enough, `NetworkTransport` drops frames rather than buffering them. This prevents latency from accumulating over time ("skipping" behaviour).
- **Thermal throttling:** The iOS app monitors `ProcessInfo.thermalState` and reduces the encoder bitrate to 50% (Serious) or 25% (Critical) of the nominal setting.
- **Jitter buffer latency budget:**
  - Low Latency mode: 0 frames held — output immediately on arrival.
  - Stable mode: 3 frames held — allows reordering of up to 3 out-of-order packets before forcing eviction.

---

## Contributing

1. Fork the repository.
2. Create a feature branch: `git checkout -b feature/your-feature`.
3. Commit your changes with clear messages.
4. Ensure all tests pass:
   ```bash
   # Swift protocol/packetisation tests
   (cd CamsApp && swift test)

   # iOS app build
   (cd CamsApp && xcodegen generate && xcodebuild -project Cams.xcodeproj -scheme Cams -configuration Debug -destination 'generic/platform=iOS' build CODE_SIGNING_ALLOWED=NO)

   # OBS plugin unit tests
   (cd obs-plugin && cmake -S . -B build -DBUILD_PLUGIN=OFF -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build)
   ```
5. Open a pull request.

---

## Release Process

Use `docs/RELEASE_CHECKLIST.md` before cutting a release tag. It includes CI gates,
plugin load checks in OBS, and runtime validation requirements.

---

## License

This project is released under the [MIT License](LICENSE).

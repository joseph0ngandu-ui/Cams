# Cams Release Checklist

Use this checklist before cutting a release tag.

## 1) Code and CI

- [ ] `swift test` passes in `CamsApp`.
- [ ] `xcodegen generate` succeeds in `CamsApp`.
- [ ] `xcodebuild -project Cams.xcodeproj -scheme Cams -configuration Debug -destination 'generic/platform=iOS' build CODE_SIGNING_ALLOWED=NO` passes in `CamsApp`.
- [ ] `cmake -S . -B build -DBUILD_PLUGIN=OFF -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release` succeeds in `obs-plugin`.
- [ ] `cmake --build build --parallel` succeeds in `obs-plugin`.
- [ ] `ctest --test-dir build --output-on-failure` passes in `obs-plugin`.
- [ ] GitHub Actions workflows are green on default branch.

## 2) Plugin build verification

- [ ] Plugin builds with `BUILD_PLUGIN=ON` on at least one macOS machine.
- [ ] Plugin loads in OBS and source type `Cams iOS Camera` appears.
- [ ] Device discovery works via Bonjour.
- [ ] Manual IP activation works when Bonjour is unavailable.
- [ ] Focus/exposure lock commands work from OBS to iOS app.
- [ ] Keyframe request works from OBS to iOS app.

## 3) Runtime quality checks

- [ ] 1080p30 stream stable for at least 15 minutes in Stable buffer mode.
- [ ] 4K stream tested on a supported device.
- [ ] Latency mode toggle (`Low Latency` / `Stable`) verified.
- [ ] Encoder fallback (HEVC -> H.264) verified.
- [ ] Thermal throttling behavior observed and documented.
- [ ] Stop/start and reconnect behavior verified.
- [ ] Audio controls are absent; video-only v1 does not claim audio support.
- [ ] Measured latency recorded with device, OBS host, Wi-Fi network, resolution, codec, and buffer mode.

## 4) Docs and metadata

- [ ] `README.md` build/install steps are still accurate.
- [ ] `README.md` protocol header size and fragmentation semantics match Swift/C++ constants.
- [ ] Known limitations include video-only v1 and any unverified latency/performance claims.
- [ ] `LICENSE` exists and matches README license statement.
- [ ] Version number updated in plugin/app where relevant.
- [ ] Release notes include known limitations and tested platforms.

## 5) Distribution sanity

- [ ] Install path verified for target platform:
  - macOS: `~/Library/Application Support/obs-studio/plugins/obs-cams/bin`
  - Linux: `~/.config/obs-studio/plugins/obs-cams/bin/64bit`
  - Windows: `C:\\Program Files\\obs-studio\\obs-plugins\\64bit`
- [ ] Fresh OBS restart confirms plugin loads without errors.

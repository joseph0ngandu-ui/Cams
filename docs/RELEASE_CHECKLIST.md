# Cams Release Checklist

Use this checklist before cutting a release tag.

## 1) Code and CI

- [ ] `swift build -c release` passes in `CamsApp`.
- [ ] `swift test` passes in `CamsApp`.
- [ ] `cmake -B build -DBUILD_PLUGIN=OFF -DBUILD_TESTS=ON` succeeds in `obs-plugin`.
- [ ] `cmake --build build --parallel` succeeds in `obs-plugin`.
- [ ] `ctest --test-dir build --output-on-failure` passes in `obs-plugin`.
- [ ] GitHub Actions workflows are green on default branch.

## 2) Plugin build verification

- [ ] Plugin builds with `BUILD_PLUGIN=ON` on at least one macOS machine.
- [ ] Plugin loads in OBS and source type `Cams iOS Camera` appears.
- [ ] Device discovery works via Bonjour.
- [ ] Focus/exposure lock commands work from OBS to iOS app.

## 3) Runtime quality checks

- [ ] 1080p stream stable for at least 15 minutes.
- [ ] 4K stream tested on a supported device.
- [ ] Latency mode toggle (`Low Latency` / `Stable`) verified.
- [ ] Encoder fallback (HEVC -> H.264) verified.
- [ ] Thermal throttling behavior observed and documented.

## 4) Docs and metadata

- [ ] `README.md` build/install steps are still accurate.
- [ ] `LICENSE` exists and matches README license statement.
- [ ] Version number updated in plugin/app where relevant.
- [ ] Release notes include known limitations and tested platforms.

## 5) Distribution sanity

- [ ] Install path verified for target platform:
  - macOS: `~/Library/Application Support/obs-studio/plugins/obs-cams/bin`
  - Linux: `~/.config/obs-studio/plugins/obs-cams/bin/64bit`
  - Windows: `C:\\Program Files\\obs-studio\\obs-plugins\\64bit`
- [ ] Fresh OBS restart confirms plugin loads without errors.

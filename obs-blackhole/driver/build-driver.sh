#!/usr/bin/env bash
#
# build-driver.sh — build the OBS-branded "OBS Audio" virtual audio driver
# from the upstream BlackHole source, then (optionally) install it.
#
# The fork is expressed entirely as compiler overrides (see OBSAudioConfig.h),
# so we never modify BlackHole's source tree.
#
# Usage:
#   ./build-driver.sh            # build only -> ./build/OBS Audio.driver
#   ./build-driver.sh --install  # build, then sudo-install + reload coreaudiod
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BH_SRC="${BLACKHOLE_SRC:-/Users/josephngandu/Dev/Cams/.deps/BlackHole-src}"
CONFIG_HEADER="$HERE/OBSAudioConfig.h"
BUILD_DIR="$HERE/build"

DRIVER_NAME="OBS Audio"
BUNDLE_ID="com.obsproject.obs-audio-driver"
HAL_DIR="/Library/Audio/Plug-Ins/HAL"

if [[ ! -d "$BH_SRC" ]]; then
  echo "error: BlackHole source not found at: $BH_SRC" >&2
  echo "       set BLACKHOLE_SRC=/path/to/BlackHole" >&2
  exit 1
fi

echo "==> Building \"$DRIVER_NAME.driver\" from $BH_SRC"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

xcodebuild \
  -project "$BH_SRC/BlackHole.xcodeproj" \
  -target BlackHole \
  -configuration Release \
  CONFIGURATION_BUILD_DIR="$BUILD_DIR" \
  PRODUCT_NAME="$DRIVER_NAME" \
  PRODUCT_BUNDLE_IDENTIFIER="$BUNDLE_ID" \
  OTHER_CFLAGS="-include $CONFIG_HEADER" \
  CODE_SIGN_IDENTITY="-" \
  CODE_SIGNING_REQUIRED=NO \
  CODE_SIGNING_ALLOWED=NO \
  clean build

DRIVER="$BUILD_DIR/$DRIVER_NAME.driver"
if [[ ! -d "$DRIVER" ]]; then
  echo "error: build did not produce $DRIVER" >&2
  exit 1
fi

# Ad-hoc sign so CoreAudio will load it locally.
echo "==> Ad-hoc signing"
codesign --force --deep --sign - "$DRIVER"
codesign --verify --verbose "$DRIVER" || true

echo
echo "Built: $DRIVER"
echo "  Device name : $DRIVER_NAME"
echo "  Bundle ID   : $BUNDLE_ID"
echo "  CoreAudio UID: OBSAudio_UID"

if [[ "${1:-}" == "--install" ]]; then
  echo
  echo "==> Installing to $HAL_DIR (requires sudo)"
  sudo rm -rf "$HAL_DIR/$DRIVER_NAME.driver"
  sudo cp -R "$DRIVER" "$HAL_DIR/"
  sudo chown -R root:wheel "$HAL_DIR/$DRIVER_NAME.driver"
  echo "==> Reloading coreaudiod"
  sudo killall coreaudiod || true
  echo "Done. 'OBS Audio' should now appear in audio device lists."
else
  echo
  echo "Not installed. To install (no reboot needed):"
  echo "  sudo cp -R \"$DRIVER\" \"$HAL_DIR/\""
  echo "  sudo killall coreaudiod"
  echo "Or re-run: $0 --install"
fi

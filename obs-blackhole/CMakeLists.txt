# CMakeLists.txt — obs-blackhole
#
# An OBS Studio plugin (macOS) that adds a "BlackHole" dock for routing OBS
# audio into the "OBS Audio" virtual microphone via CoreAudio.
#
# Configure (defaults point at this repo's vendored deps):
#   cmake -B build -G Ninja
#   cmake --build build
#   cmake --install build      # -> ~/Library/Application Support/obs-studio/plugins
#
cmake_minimum_required(VERSION 3.21)
project(obs-blackhole VERSION 1.0.0 LANGUAGES C CXX)

if(NOT APPLE)
  message(FATAL_ERROR "obs-blackhole is macOS-only (CoreAudio).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS")

set(CMAKE_AUTOMOC ON)

# --- Vendored dependency locations (override with -D… as needed) -------------
set(_DEV "/Users/josephngandu/Dev/Cams")
set(OBS_SDK_ROOT   "${_DEV}/.deps/obs-sdk"               CACHE PATH "libobs SDK root")
set(OBS_SRC_ROOT   "${_DEV}/.deps/obs-studio"            CACHE PATH "OBS source (frontend-api header)")
set(OBS_APP        "/Applications/OBS.app"               CACHE PATH "Installed OBS.app (runtime libs)")
set(QT_ROOT        "${_DEV}/.deps/Qt/6.8.3/macos"        CACHE PATH "Qt 6.8.3 root")

list(PREPEND CMAKE_PREFIX_PATH "${QT_ROOT}")
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

# --- libobs -----------------------------------------------------------------
find_path(OBS_INCLUDE_DIR obs.h HINTS "${OBS_SDK_ROOT}/include/obs" REQUIRED)
find_library(OBS_LIB obs libobs HINTS "${OBS_SDK_ROOT}/lib" REQUIRED)

# --- obs-frontend-api (header in source tree, dylib in the installed app) ----
find_path(OBS_FRONTEND_INCLUDE_DIR obs-frontend-api.h
  HINTS "${OBS_SRC_ROOT}/frontend/api" REQUIRED)
# The shipped file has no "lib" prefix, so find_library won't match it; look for
# the exact filename and fall back to a direct path.
find_file(OBS_FRONTEND_LIB
  NAMES obs-frontend-api.dylib libobs-frontend-api.1.dylib
  HINTS "${OBS_APP}/Contents/Frameworks")
if(NOT OBS_FRONTEND_LIB)
  set(OBS_FRONTEND_LIB "${OBS_APP}/Contents/Frameworks/obs-frontend-api.dylib")
endif()
message(STATUS "obs-frontend-api: ${OBS_FRONTEND_LIB}")

# --- vendored RNNoise (noise suppression) -----------------------------------
# Self-contained BSD C library (model weights baked into rnn_data.c). OBS links
# the same code into obs-filters, but those symbols aren't exported by the host
# process, so we compile our own copy in. Built as an OBJECT lib so we can apply
# its own (warning-suppressed) compile settings.
set(RNNOISE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/rnnoise")
add_library(obs-blackhole-rnnoise OBJECT
  "${RNNOISE_DIR}/src/celt_lpc.c"
  "${RNNOISE_DIR}/src/denoise.c"
  "${RNNOISE_DIR}/src/kiss_fft.c"
  "${RNNOISE_DIR}/src/pitch.c"
  "${RNNOISE_DIR}/src/rnn.c"
  "${RNNOISE_DIR}/src/rnn_data.c"
  "${RNNOISE_DIR}/src/rnn_reader.c"
)
target_include_directories(obs-blackhole-rnnoise PUBLIC "${RNNOISE_DIR}/include")
target_compile_definitions(obs-blackhole-rnnoise PUBLIC COMPILE_OPUS)
target_compile_options(obs-blackhole-rnnoise PRIVATE -w) # third-party C
set_target_properties(obs-blackhole-rnnoise PROPERTIES POSITION_INDEPENDENT_CODE TRUE)

# --- target -----------------------------------------------------------------
add_library(obs-blackhole MODULE
  src/plugin-main.cpp
  src/BlackHoleDock.cpp
  src/VirtualMicEngine.cpp
  src/AudioDeviceControl.cpp
  src/AudioRouterEngine.cpp
  src/NoiseSuppressor.cpp
  src/AudioDsp.cpp
  src/RingBuffer.h
  src/BlackHoleDock.h
  src/VirtualMicEngine.h
  src/AudioRouterEngine.h
  src/AudioDeviceControl.h
  src/NoiseSuppressor.h
  src/AudioDsp.h
)

target_include_directories(obs-blackhole PRIVATE
  src
  "${RNNOISE_DIR}/include"
  "${OBS_INCLUDE_DIR}"
  "${OBS_FRONTEND_INCLUDE_DIR}"
)

# Qt: use the imported targets ONLY for include dirs + AUTOMOC. We deliberately
# do NOT link the Qt targets, because their transitive interface drags in the
# legacy 'AGL' framework (removed from modern macOS SDKs). OBS already has Qt
# 6.8.3 loaded in-process, so our Qt symbols resolve at load via dynamic_lookup.
target_include_directories(obs-blackhole SYSTEM PRIVATE
  $<TARGET_PROPERTY:Qt6::Core,INTERFACE_INCLUDE_DIRECTORIES>
  $<TARGET_PROPERTY:Qt6::Gui,INTERFACE_INCLUDE_DIRECTORIES>
  $<TARGET_PROPERTY:Qt6::Widgets,INTERFACE_INCLUDE_DIRECTORIES>
)
target_compile_definitions(obs-blackhole PRIVATE
  $<TARGET_PROPERTY:Qt6::Core,INTERFACE_COMPILE_DEFINITIONS>
)

target_link_libraries(obs-blackhole PRIVATE
  obs-blackhole-rnnoise
  "${OBS_LIB}"
  "${OBS_FRONTEND_LIB}"
  "-framework CoreAudio"
  "-framework AudioToolbox"
  "-framework AudioUnit"
  "-framework CoreFoundation"
  "-framework Foundation"
)

# OBS already has libobs/Qt loaded in-process; resolve our undefined symbols
# (incl. all Qt) against the host at load time rather than re-bundling anything.
target_link_options(obs-blackhole PRIVATE
  "-Wl,-undefined,dynamic_lookup"
)

target_compile_options(obs-blackhole PRIVATE
  -Wall -Wextra -Wno-unused-parameter
  $<$<CONFIG:Release>:-O2>
)

# --- macOS .plugin bundle ---------------------------------------------------
set_target_properties(obs-blackhole PROPERTIES
  BUNDLE TRUE
  BUNDLE_EXTENSION "plugin"
  OUTPUT_NAME "obs-blackhole"
  MACOSX_BUNDLE_BUNDLE_NAME "obs-blackhole"
  MACOSX_BUNDLE_GUI_IDENTIFIER "com.obsproject.obs-blackhole"
  MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
)

# --- install ----------------------------------------------------------------
set(OBS_PLUGIN_DEST "$ENV{HOME}/Library/Application Support/obs-studio/plugins"
    CACHE PATH "OBS plugin directory")
install(TARGETS obs-blackhole
  BUNDLE  DESTINATION "${OBS_PLUGIN_DEST}"
  LIBRARY DESTINATION "${OBS_PLUGIN_DEST}")

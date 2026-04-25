# CMakeLists.txt — Cams OBS Plugin
# Supports: macOS (arm64 / x86_64), Linux (x86_64 / arm64), Windows (x86_64)
#
# Prerequisites:
#   • CMake 3.20+
#   • OBS Studio SDK (libobs)
#   • FFmpeg (libavcodec, libavutil, libavformat)
#   • On macOS: Xcode Command Line Tools (for dns_sd / VideoToolbox)
#   • On Linux: Avahi compat-libdns_sd (optional, for Bonjour discovery)
#
# Quick start:
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --parallel

cmake_minimum_required(VERSION 3.20)
project(obs-cams VERSION 1.0.0 LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Language standard
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Architecture detection
# ---------------------------------------------------------------------------
if(APPLE)
    # Universal binary — compile for both Apple Silicon and Intel.
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS target architectures")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
endif()

# ---------------------------------------------------------------------------
# Search paths (override these on your system)
# ---------------------------------------------------------------------------

# OBS Studio SDK root.  Set via -DOBS_SDK_ROOT=/path/to/obs if not installed.
set(OBS_SDK_ROOT "" CACHE PATH "Path to OBS Studio SDK root")

# FFmpeg root.  Set via -DFFMPEG_ROOT=/path/to/ffmpeg if not in system paths.
set(FFMPEG_ROOT "" CACHE PATH "Path to FFmpeg root (contains lib/ and include/)")

# ---------------------------------------------------------------------------
# Find OBS (not required when only building tests)
# ---------------------------------------------------------------------------
option(BUILD_PLUGIN "Build the OBS plugin module (requires OBS SDK and FFmpeg)" ON)

if(BUILD_PLUGIN)
    if(OBS_SDK_ROOT)
        list(APPEND CMAKE_PREFIX_PATH "${OBS_SDK_ROOT}")
    endif()

    find_package(libobs QUIET)

    if(NOT libobs_FOUND)
        # Fallback: manual search.
        find_path(OBS_INCLUDE_DIR obs.h
            HINTS
                "${OBS_SDK_ROOT}/include"
                "${OBS_SDK_ROOT}/include/obs"
                /usr/include/obs
                /usr/local/include/obs
                "C:/Program Files/obs-studio/include"
            PATH_SUFFIXES obs
        )
        find_library(OBS_LIBRARY
            NAMES obs libobs
            HINTS
                "${OBS_SDK_ROOT}/lib"
                /usr/lib
                /usr/local/lib
                "C:/Program Files/obs-studio/bin/64bit"
        )
        if(NOT OBS_INCLUDE_DIR OR NOT OBS_LIBRARY)
            message(STATUS
                "[Cams] OBS Studio SDK not found — plugin module will not be built. "
                "Set -DOBS_SDK_ROOT=/path/to/obs-studio to enable it."
            )
            set(BUILD_PLUGIN OFF CACHE BOOL "" FORCE)
        else()
            add_library(OBS::libobs UNKNOWN IMPORTED)
            set_target_properties(OBS::libobs PROPERTIES
                IMPORTED_LOCATION "${OBS_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${OBS_INCLUDE_DIR}"
            )
        endif()
    else()
        add_library(OBS::libobs ALIAS libobs)
    endif()
endif()

# ---------------------------------------------------------------------------
# Find FFmpeg and build the plugin target (only when BUILD_PLUGIN is ON)
# ---------------------------------------------------------------------------
if(BUILD_PLUGIN)
    if(FFMPEG_ROOT)
        list(APPEND CMAKE_PREFIX_PATH "${FFMPEG_ROOT}")
        list(APPEND PKG_CONFIG_PATH  "${FFMPEG_ROOT}/lib/pkgconfig")
        set(ENV{PKG_CONFIG_PATH} "${FFMPEG_ROOT}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    endif()

    find_package(PkgConfig QUIET)

    if(PkgConfig_FOUND)
        pkg_check_modules(AVCODEC  QUIET libavcodec)
        pkg_check_modules(AVUTIL   QUIET libavutil)
        pkg_check_modules(AVFORMAT QUIET libavformat)
        if(NOT AVCODEC_FOUND OR NOT AVUTIL_FOUND OR NOT AVFORMAT_FOUND)
            message(STATUS "[Cams] FFmpeg not found via pkg-config — plugin will not be built.")
            set(BUILD_PLUGIN OFF CACHE BOOL "" FORCE)
        endif()
    else()
        # Manual fallback.
        set(_ff_all_found TRUE)
        foreach(_lib avcodec avutil avformat)
            find_library(FF_${_lib} NAMES ${_lib}
                HINTS "${FFMPEG_ROOT}/lib" /usr/lib /usr/local/lib
            )
            if(NOT FF_${_lib})
                message(STATUS "[Cams] FFmpeg lib${_lib} not found — plugin will not be built.")
                set(_ff_all_found FALSE)
            endif()
        endforeach()
        if(NOT _ff_all_found)
            set(BUILD_PLUGIN OFF CACHE BOOL "" FORCE)
        else()
            find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h
                HINTS "${FFMPEG_ROOT}/include" /usr/include /usr/local/include
            )
        endif()
    endif()
endif()

if(BUILD_PLUGIN)
# ---------------------------------------------------------------------------
# Plugin target
# ---------------------------------------------------------------------------
set(CAMS_SOURCES
    src/plugin-main.cpp
    src/CamsSource.cpp
    src/FrameReassembler.cpp
    src/JitterBuffer.cpp
    src/NetworkListener.cpp
    src/VideoDecoder.cpp
    src/ControlServer.cpp
)

set(CAMS_HEADERS
    src/CamsPacket.h
    src/CamsSource.h
    src/FrameReassembler.h
    src/JitterBuffer.h
    src/NetworkListener.h
    src/VideoDecoder.h
    src/ControlServer.h
)

add_library(obs-cams MODULE ${CAMS_SOURCES} ${CAMS_HEADERS})

# On macOS OBS plugins must not have the "lib" prefix.
set_target_properties(obs-cams PROPERTIES PREFIX "")

target_include_directories(obs-cams PRIVATE src)

# Link OBS
target_link_libraries(obs-cams PRIVATE OBS::libobs)

# Link FFmpeg
if(PkgConfig_FOUND)
    target_include_directories(obs-cams PRIVATE
        ${AVCODEC_INCLUDE_DIRS}
        ${AVUTIL_INCLUDE_DIRS}
        ${AVFORMAT_INCLUDE_DIRS}
    )
    target_link_libraries(obs-cams PRIVATE
        ${AVCODEC_LIBRARIES}
        ${AVUTIL_LIBRARIES}
        ${AVFORMAT_LIBRARIES}
    )
    target_compile_options(obs-cams PRIVATE
        ${AVCODEC_CFLAGS_OTHER}
    )
else()
    target_include_directories(obs-cams PRIVATE "${FFMPEG_INCLUDE_DIR}")
    target_link_libraries(obs-cams PRIVATE
        ${FF_avcodec} ${FF_avutil} ${FF_avformat}
    )
endif()

# ---------------------------------------------------------------------------
# Platform-specific settings
# ---------------------------------------------------------------------------
if(APPLE)
    # Link Bonjour (dns_sd) — built-in on macOS.
    target_link_libraries(obs-cams PRIVATE "-framework CoreFoundation")
    # Strip unnecessary symbols in release builds.
    target_link_options(obs-cams PRIVATE $<$<CONFIG:Release>:-dead_strip>)
    # ARM64 optimisation flags.
    target_compile_options(obs-cams PRIVATE
        $<$<CONFIG:Release>:-O3 -ffast-math>
    )
elseif(UNIX)
    # Linux: optional Avahi dns_sd compatibility.
    find_library(AVAHI_DNS_SD avahi-compat-libdns_sd)
    if(AVAHI_DNS_SD)
        target_link_libraries(obs-cams PRIVATE ${AVAHI_DNS_SD})
        target_compile_definitions(obs-cams PRIVATE CAMS_HAS_DNSSD=1)
    endif()
    target_compile_options(obs-cams PRIVATE
        $<$<CONFIG:Release>:-O3 -march=native -ffast-math>
    )
elseif(WIN32)
    target_link_libraries(obs-cams PRIVATE ws2_32)
    target_compile_definitions(obs-cams PRIVATE
        _WIN32_WINNT=0x0A00   # Windows 10
        NOMINMAX
    )
    target_compile_options(obs-cams PRIVATE
        $<$<CONFIG:Release>:/O2 /GL>
    )
    target_link_options(obs-cams PRIVATE $<$<CONFIG:Release>:/LTCG>)
endif()

# ---------------------------------------------------------------------------
# Compiler warnings
# ---------------------------------------------------------------------------
if(MSVC)
    target_compile_options(obs-cams PRIVATE /W4 /WX /wd4100 /wd4505)
else()
    target_compile_options(obs-cams PRIVATE
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
        -Wno-missing-field-initializers
    )
endif()

# ---------------------------------------------------------------------------
# Installation
# ---------------------------------------------------------------------------
if(APPLE)
    set(OBS_PLUGIN_INSTALL_DIR
        "$ENV{HOME}/Library/Application Support/obs-studio/plugins/obs-cams/bin"
        CACHE PATH "Plugin install directory")
elseif(UNIX)
    set(OBS_PLUGIN_INSTALL_DIR
        "$ENV{HOME}/.config/obs-studio/plugins/obs-cams/bin/64bit"
        CACHE PATH "Plugin install directory")
elseif(WIN32)
    set(OBS_PLUGIN_INSTALL_DIR
        "C:/Program Files/obs-studio/obs-plugins/64bit"
        CACHE PATH "Plugin install directory")
endif()

install(TARGETS obs-cams DESTINATION "${OBS_PLUGIN_INSTALL_DIR}")

endif() # BUILD_PLUGIN

# ---------------------------------------------------------------------------
# Unit tests (standalone — no OBS required)
# ---------------------------------------------------------------------------
option(BUILD_TESTS "Build standalone unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_executable(cams_tests
        tests/JitterBufferTest.cpp
        src/ControlServer.cpp
        src/FrameReassembler.cpp
        src/JitterBuffer.cpp
    )
    target_include_directories(cams_tests PRIVATE src)
    target_compile_features(cams_tests PRIVATE cxx_std_17)
    if(WIN32)
        target_link_libraries(cams_tests PRIVATE ws2_32)
    endif()
    add_test(NAME cams_tests COMMAND cams_tests)
endif()

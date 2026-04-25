// CamsSource.h
// Cams OBS Plugin — obs_source_info implementation.
//
// Ties together:
//  • NetworkListener  — receives UDP video packets
//  • JitterBuffer     — reorders packets
//  • VideoDecoder     — FFmpeg hardware decode
//  • ControlServer    — back-channel for camera commands
//  • OBS video output — pushes obs_source_frame to the pipeline

#pragma once

#include "CamsPacket.h"
#include "ControlServer.h"
#include "JitterBuffer.h"
#include "NetworkListener.h"
#include "VideoDecoder.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

extern "C" {
#include <obs-module.h>
}

namespace cams {

// ---------------------------------------------------------------------------
// BufferMode
// ---------------------------------------------------------------------------

enum class BufferMode {
    LowLatency = 0,   ///< 0-frame jitter buffer — minimum delay.
    Stable     = 3,   ///< 3-frame jitter buffer — tolerates packet reordering.
};

// ---------------------------------------------------------------------------
// CamsSource
// ---------------------------------------------------------------------------

class CamsSource {
public:
    explicit CamsSource(obs_data_t *settings, obs_source_t *source);
    ~CamsSource();

    // -----------------------------------------------------------------------
    // obs_source_info callbacks

    static const char *getName(void *) { return "Cams iOS Camera"; }
    static void       *create(obs_data_t *settings, obs_source_t *source);
    static void        destroy(void *data);
    static void        getDefaults(obs_data_t *settings);
    static obs_properties_t *getProperties(void *data);
    static void        update(void *data, obs_data_t *settings);
    static uint32_t    getWidth(void *data);
    static uint32_t    getHeight(void *data);

private:
    obs_source_t *m_source  = nullptr;
    uint32_t      m_width   = 3840;
    uint32_t      m_height  = 2160;

    std::unique_ptr<NetworkListener> m_listener;
    std::unique_ptr<JitterBuffer>    m_jitterBuffer;
    std::unique_ptr<VideoDecoder>    m_decoder;
    std::unique_ptr<ControlServer>   m_controlServer;

    std::thread       m_decoderThread;
    std::atomic<bool> m_running{false};
    FrameType         m_decoderCodec = FrameType::H264; // Codec the decoder was opened with.

    // Selected device info (from the properties UI).
    std::string m_selectedDevice;
    std::string m_manualHost;
    BufferMode  m_bufferMode = BufferMode::Stable;
    int         m_qualityPreset = 2; // 0 low, 1 medium, 2 high
    bool        m_audioEnabled = false;

    // ── OBS frame output ─────────────────────────────────────────────────
    obs_source_frame m_obsFrame{};

    // ── Internals ────────────────────────────────────────────────────────
    void applySettings(obs_data_t *settings);
    void activateTarget();
    void startPipeline();
    void stopPipeline();
    void decoderLoop();
    void outputAVFrame(AVFrame *frame);

    // ── Properties callbacks ─────────────────────────────────────────────
    static bool onFocusLockClicked(
        obs_properties_t *props, obs_property_t *prop, void *data);
    static bool onExposureLockClicked(
        obs_properties_t *props, obs_property_t *prop, void *data);
    static bool onRefreshDevicesClicked(
        obs_properties_t *props, obs_property_t *prop, void *data);
    static bool onActivateClicked(
        obs_properties_t *props, obs_property_t *prop, void *data);
};

} // namespace cams

// ---------------------------------------------------------------------------
// C-linkage registration helper (called from plugin-main.cpp)
// ---------------------------------------------------------------------------
void cams_register_source();

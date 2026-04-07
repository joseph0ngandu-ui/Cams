// CamsSource.cpp
// Cams OBS Plugin — obs_source_info implementation.

#include "CamsSource.h"

#include <cstring>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <media-io/video-io.h>
#include <obs-module.h>
}

namespace cams {

// ---------------------------------------------------------------------------
// Settings key constants
// ---------------------------------------------------------------------------

static constexpr const char *kSettingDevice     = "device";
static constexpr const char *kSettingBufferMode = "buffer_mode";
static constexpr const char *kSettingFocusLock  = "focus_lock";
static constexpr const char *kSettingExpLock    = "exposure_lock";

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

CamsSource::CamsSource(obs_data_t *settings, obs_source_t *source)
    : m_source(source)
{
    m_listener      = std::make_unique<NetworkListener>();
    m_jitterBuffer  = std::make_unique<JitterBuffer>(3);
    m_decoder       = std::make_unique<VideoDecoder>();
    m_controlServer = std::make_unique<ControlServer>();

    std::memset(&m_obsFrame, 0, sizeof(m_obsFrame));

    applySettings(settings);
    startPipeline();
}

CamsSource::~CamsSource() {
    stopPipeline();
}

// ---------------------------------------------------------------------------
// obs_source_info factory
// ---------------------------------------------------------------------------

void *CamsSource::create(obs_data_t *settings, obs_source_t *source) {
    try {
        return new CamsSource(settings, source);
    } catch (const std::exception &ex) {
        blog(LOG_ERROR, "[Cams] CamsSource::create exception: %s", ex.what());
        return nullptr;
    }
}

void CamsSource::destroy(void *data) {
    delete static_cast<CamsSource *>(data);
}

// ---------------------------------------------------------------------------
// Default settings
// ---------------------------------------------------------------------------

void CamsSource::getDefaults(obs_data_t *settings) {
    obs_data_set_default_int(settings, kSettingBufferMode,
                             static_cast<int>(BufferMode::Stable));
    obs_data_set_default_bool(settings, kSettingFocusLock, false);
    obs_data_set_default_bool(settings, kSettingExpLock,   false);
}

// ---------------------------------------------------------------------------
// Properties UI
// ---------------------------------------------------------------------------

obs_properties_t *CamsSource::getProperties(void *data) {
    auto *self  = static_cast<CamsSource *>(data);
    auto *props = obs_properties_create();

    // ── Device list ───────────────────────────────────────────────────────
    auto *deviceList = obs_properties_add_list(
        props, kSettingDevice, "iOS Device",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING
    );

    // Populate with currently discovered Bonjour devices.
    if (self) {
        for (const auto &dev : self->m_listener->discoveredDevices()) {
            obs_property_list_add_string(
                deviceList, dev.name.c_str(), dev.hostName.c_str());
        }
    }

    // Refresh button.
    obs_properties_add_button(
        props, "refresh_devices", "Refresh Devices",
        &CamsSource::onRefreshDevicesClicked
    );

    // ── Buffer mode ───────────────────────────────────────────────────────
    auto *bufList = obs_properties_add_list(
        props, kSettingBufferMode, "Buffer Mode",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT
    );
    obs_property_list_add_int(bufList, "Low Latency (0 frames)",
                              static_cast<int>(BufferMode::LowLatency));
    obs_property_list_add_int(bufList, "Stable (3 frames)",
                              static_cast<int>(BufferMode::Stable));

    // ── Camera controls ───────────────────────────────────────────────────
    obs_properties_add_button(
        props, kSettingFocusLock, "Toggle Focus Lock",
        &CamsSource::onFocusLockClicked
    );
    obs_properties_add_button(
        props, kSettingExpLock, "Toggle Exposure Lock",
        &CamsSource::onExposureLockClicked
    );

    return props;
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------

void CamsSource::update(void *data, obs_data_t *settings) {
    auto *self = static_cast<CamsSource *>(data);
    if (!self) return;
    self->applySettings(settings);
}

void CamsSource::applySettings(obs_data_t *settings) {
    const char *device = obs_data_get_string(settings, kSettingDevice);
    if (device) m_selectedDevice = device;

    auto mode = static_cast<BufferMode>(
        obs_data_get_int(settings, kSettingBufferMode));
    if (mode != m_bufferMode) {
        m_bufferMode = mode;
        m_jitterBuffer->setCapacity(static_cast<size_t>(m_bufferMode));
    }

    // Update the control server target whenever the device changes.
    if (!m_selectedDevice.empty()) {
        m_controlServer->setTarget(m_selectedDevice, kControlPort);
    }
}

// ---------------------------------------------------------------------------
// Dimensions
// ---------------------------------------------------------------------------

uint32_t CamsSource::getWidth(void *data) {
    return data ? static_cast<CamsSource *>(data)->m_width : 0;
}

uint32_t CamsSource::getHeight(void *data) {
    return data ? static_cast<CamsSource *>(data)->m_height : 0;
}

// ---------------------------------------------------------------------------
// Pipeline management
// ---------------------------------------------------------------------------

void CamsSource::startPipeline() {
    if (m_running.load()) return;

    // Open decoder (HEVC preferred; will fall back to H.264 as needed).
    if (!m_decoder->open(FrameType::HEVC)) {
        if (!m_decoder->open(FrameType::H264)) {
            blog(LOG_ERROR, "[Cams] Failed to open video decoder");
            return;
        }
    }

    // Wire the network listener → jitter buffer.
    m_listener->setPacketCallback(
        [this](PacketHeader hdr, std::vector<uint8_t> payload) {
            if (isVideoData(hdr.frameType)) {
                m_jitterBuffer->push(hdr, std::move(payload));
            }
        }
    );

    if (!m_listener->start(kVideoPort)) {
        blog(LOG_ERROR, "[Cams] NetworkListener failed to start on port %u", kVideoPort);
        return;
    }

    m_listener->startDiscovery();

    if (!m_controlServer->start(kControlPort)) {
        blog(LOG_WARNING, "[Cams] ControlServer failed to start (non-fatal)");
    }

    m_running.store(true);
    m_decoderThread = std::thread(&CamsSource::decoderLoop, this);
}

void CamsSource::stopPipeline() {
    m_running.store(false);
    m_jitterBuffer->flush();

    if (m_decoderThread.joinable()) m_decoderThread.join();

    m_listener->stopDiscovery();
    m_listener->stop();
    m_controlServer->stop();
    m_decoder->close();
}

// ---------------------------------------------------------------------------
// Decoder thread
// ---------------------------------------------------------------------------

void CamsSource::decoderLoop() {
    while (m_running.load()) {
        auto frameOpt = m_jitterBuffer->pop();
        if (!frameOpt) break;  // Buffer was flushed — exit cleanly.

        const auto &frame = *frameOpt;

        // Re-open the decoder if the codec changes mid-stream.
        FrameType expectedCodec =
            (frame.frameType == FrameType::HEVC ||
             frame.frameType == FrameType::ParameterSets)
            ? FrameType::HEVC : FrameType::H264;

        if (!m_decoder->isOpen()) {
            if (!m_decoder->open(expectedCodec)) {
                // Cannot decode — skip frame and request keyframe from iOS.
                m_controlServer->sendKeyframeRequest();
                continue;
            }
        }

        bool ok = m_decoder->decode(frame.data, [this](AVFrame *avFrame) {
            outputAVFrame(avFrame);
        });

        if (!ok) {
            // Decoder returned an unrecoverable error; request an IDR frame.
            blog(LOG_WARNING, "[Cams] Decoder error — requesting keyframe");
            m_controlServer->sendKeyframeRequest();
        }
    }
}

// ---------------------------------------------------------------------------
// OBS frame output
// ---------------------------------------------------------------------------

void CamsSource::outputAVFrame(AVFrame *avFrame) {
    if (!avFrame || !m_source) return;

    // Update source dimensions if needed.
    if (static_cast<uint32_t>(avFrame->width)  != m_width ||
        static_cast<uint32_t>(avFrame->height) != m_height)
    {
        m_width  = static_cast<uint32_t>(avFrame->width);
        m_height = static_cast<uint32_t>(avFrame->height);
    }

    // Map AVPixelFormat → OBS video_format.
    video_format obsFormat = VIDEO_FORMAT_NONE;
    switch (static_cast<AVPixelFormat>(avFrame->format)) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        obsFormat = VIDEO_FORMAT_I420;
        break;
    case AV_PIX_FMT_NV12:
        obsFormat = VIDEO_FORMAT_NV12;
        break;
    case AV_PIX_FMT_YUV444P:
        obsFormat = VIDEO_FORMAT_I444;
        break;
    default:
        // Unsupported pixel format — skip.
        return;
    }

    m_obsFrame.format    = obsFormat;
    m_obsFrame.width     = static_cast<uint32_t>(avFrame->width);
    m_obsFrame.height    = static_cast<uint32_t>(avFrame->height);
    m_obsFrame.timestamp = static_cast<uint64_t>(avFrame->pts);

    for (int i = 0; i < MAX_AV_PLANES; ++i) {
        m_obsFrame.data[i]     = avFrame->data[i];
        m_obsFrame.linesize[i] = static_cast<uint32_t>(avFrame->linesize[i]);
    }

    obs_source_output_video(m_source, &m_obsFrame);
}

// ---------------------------------------------------------------------------
// Property button callbacks
// ---------------------------------------------------------------------------

bool CamsSource::onFocusLockClicked(
    obs_properties_t * /*props*/,
    obs_property_t   * /*prop*/,
    void             *data
) {
    auto *self = static_cast<CamsSource *>(data);
    if (!self) return false;

    // Toggle: infer current state from settings.
    bool currentlyLocked = obs_data_get_bool(
        obs_source_get_settings(self->m_source), kSettingFocusLock);
    bool newLocked = !currentlyLocked;

    self->m_controlServer->sendFocusLock(newLocked);
    obs_data_set_bool(
        obs_source_get_settings(self->m_source), kSettingFocusLock, newLocked);
    return true;
}

bool CamsSource::onExposureLockClicked(
    obs_properties_t * /*props*/,
    obs_property_t   * /*prop*/,
    void             *data
) {
    auto *self = static_cast<CamsSource *>(data);
    if (!self) return false;

    bool currentlyLocked = obs_data_get_bool(
        obs_source_get_settings(self->m_source), kSettingExpLock);
    bool newLocked = !currentlyLocked;

    self->m_controlServer->sendExposureLock(newLocked);
    obs_data_set_bool(
        obs_source_get_settings(self->m_source), kSettingExpLock, newLocked);
    return true;
}

bool CamsSource::onRefreshDevicesClicked(
    obs_properties_t *props,
    obs_property_t   * /*prop*/,
    void             *data
) {
    auto *self = static_cast<CamsSource *>(data);
    if (!self) return false;

    obs_property_t *deviceProp = obs_properties_get(props, kSettingDevice);
    if (!deviceProp) return false;

    obs_property_list_clear(deviceProp);
    for (const auto &dev : self->m_listener->discoveredDevices()) {
        obs_property_list_add_string(
            deviceProp, dev.name.c_str(), dev.hostName.c_str());
    }
    return true;
}

} // namespace cams

// ---------------------------------------------------------------------------
// C-linkage registration
// ---------------------------------------------------------------------------

void cams_register_source() {
    static obs_source_info info{};
    info.id           = "cams_ios_source";
    info.type         = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_ASYNC_VIDEO;
    info.get_name     = cams::CamsSource::getName;
    info.create       = cams::CamsSource::create;
    info.destroy      = cams::CamsSource::destroy;
    info.get_defaults = cams::CamsSource::getDefaults;
    info.get_properties = cams::CamsSource::getProperties;
    info.update       = cams::CamsSource::update;
    info.get_width    = cams::CamsSource::getWidth;
    info.get_height   = cams::CamsSource::getHeight;
    obs_register_source(&info);
}

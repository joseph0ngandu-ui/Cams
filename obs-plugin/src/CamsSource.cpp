// CamsSource.cpp
// Cams OBS Plugin — obs_source_info implementation.

#include "CamsSource.h"

#include <cstring>
#include <limits>
#include <sstream>
#include <vector>
#ifndef _WIN32
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <media-io/video-io.h>
#include <obs-module.h>
}

namespace cams {

static FrameType detectCodecFromAnnexB(const std::vector<uint8_t> &data, FrameType fallback) {
    for (size_t i = 0; i + 4 < data.size(); ++i) {
        size_t nalOffset = std::numeric_limits<size_t>::max();
        if (i + 4 < data.size() &&
            data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
            nalOffset = i + 4;
        } else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            nalOffset = i + 3;
        }

        if (nalOffset == std::numeric_limits<size_t>::max() || nalOffset >= data.size()) continue;

        uint8_t first = data[nalOffset];
        uint8_t h264Type = first & 0x1F;
        uint8_t hevcType = (first >> 1) & 0x3F;

        if (hevcType == 19 || hevcType == 20 || hevcType == 21 ||
            hevcType == 32 || hevcType == 33 || hevcType == 34) {
            return FrameType::HEVC;
        }
        if (h264Type == 5 || h264Type == 7 || h264Type == 8) {
            return FrameType::H264;
        }
    }
    return fallback;
}

// ---------------------------------------------------------------------------
// Settings key constants
// ---------------------------------------------------------------------------

static constexpr const char *kSettingDevice     = "device";
static constexpr const char *kSettingManualHost = "manual_host";
static constexpr const char *kSettingBufferMode = "buffer_mode";
static constexpr const char *kSettingFocusLock  = "focus_lock";
static constexpr const char *kSettingExpLock    = "exposure_lock";
static constexpr const char *kSettingQuality    = "quality_mode";
static constexpr const char *kSettingAudio      = "audio_enabled";
static constexpr const char *kSettingActivate   = "activate_camera";

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

    try {
        applySettings(settings);
        startPipeline();
        activateTarget();
    } catch (...) {
        stopPipeline();
        throw;
    }
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
                             static_cast<int>(BufferMode::LowLatency));
    obs_data_set_default_string(settings, kSettingManualHost, "");
    obs_data_set_default_bool(settings, kSettingFocusLock, false);
    obs_data_set_default_bool(settings, kSettingExpLock,   false);
    obs_data_set_default_int(settings, kSettingQuality, 1);
    obs_data_set_default_bool(settings, kSettingAudio, false);
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
            std::ostringstream label;
            label << dev.name << " (" << dev.hostName << ":" << dev.port << ")";
            obs_property_list_add_string(
                deviceList, label.str().c_str(), dev.hostName.c_str());
        }
    }

    obs_properties_add_text(
        props,
        kSettingManualHost,
        "Manual iPhone IP (optional)",
        OBS_TEXT_DEFAULT
    );

    // Local binding info for manual setup.
    std::string localIP = "unknown";
#ifndef _WIN32
    ifaddrs *ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == 0 && ifaddr) {
        for (auto *p = ifaddr; p != nullptr; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
            std::string name = p->ifa_name ? p->ifa_name : "";
            if (name != "en0" && name != "eth0" && name != "wlan0") continue;
            char host[NI_MAXHOST] = {};
            if (getnameinfo(p->ifa_addr, sizeof(sockaddr_in), host, sizeof(host), nullptr, 0, NI_NUMERICHOST) == 0) {
                localIP = host;
                break;
            }
        }
        freeifaddrs(ifaddr);
    }
#endif
    std::ostringstream manualInfo;
    manualInfo << "My IP: " << localIP
               << "  |  Video Port: " << kVideoPort
               << "  |  Control Port: " << kControlPort;
    obs_properties_add_text(props, "manual_info", manualInfo.str().c_str(), OBS_TEXT_INFO);

    // Refresh button.
    obs_properties_add_button(
        props, "refresh_devices", "Refresh Devices",
        &CamsSource::onRefreshDevicesClicked
    );
    obs_properties_add_button(
        props, kSettingActivate, "Activate Camera",
        &CamsSource::onActivateClicked
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

    auto *qualityList = obs_properties_add_list(
        props, kSettingQuality, "Video Quality",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT
    );
    obs_property_list_add_int(qualityList, "Low (720p30)", 0);
    obs_property_list_add_int(qualityList, "Medium (1080p30)", 1);
    obs_property_list_add_int(qualityList, "High (4K60)", 2);

    obs_properties_add_bool(props, kSettingAudio, "Enable Audio (iOS)");

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
    const char *manual = obs_data_get_string(settings, kSettingManualHost);
    if (device) m_selectedDevice = device;
    if (manual) m_manualHost = manual;

    auto mode = static_cast<BufferMode>(
        obs_data_get_int(settings, kSettingBufferMode));
    if (mode != m_bufferMode) {
        m_bufferMode = mode;
        m_jitterBuffer->setCapacity(static_cast<size_t>(m_bufferMode));
    }

    // Auto-activate: when a target is set, send the full activation sequence
    // so the user doesn't need to click a separate "Activate Camera" button.
    m_qualityPreset = static_cast<int>(obs_data_get_int(settings, kSettingQuality));
    m_audioEnabled  = obs_data_get_bool(settings, kSettingAudio);

    activateTarget();
}

void CamsSource::activateTarget() {
    const std::string targetHost = !m_manualHost.empty() ? m_manualHost : m_selectedDevice;
    if (targetHost.empty()) return;

    m_controlServer->setTarget(targetHost, kControlPort);
    m_controlServer->sendQuality(m_qualityPreset);
    m_controlServer->sendAudioEnabled(m_audioEnabled);
    m_controlServer->sendKeyframeRequest();
    blog(LOG_INFO, "[Cams] Activated target %s:%u", targetHost.c_str(), kControlPort);
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

    // Don't pre-open the decoder — let the decoder loop open it on the first
    // frame so we use the codec the iOS app is actually sending.  This avoids
    // the fatal HEVC-decoder-vs-H.264-data mismatch.
    m_decoderCodec = FrameType::H264; // Will be overwritten on first frame.

    // Wire the network listener → jitter buffer.
    m_listener->setPacketCallback(
        [this](PacketHeader hdr, std::vector<uint8_t> payload) {
            if (isVideoData(hdr.frameType)) {
                m_jitterBuffer->push(hdr, std::move(payload));
            }
        }
    );
    m_listener->setLossCallback(
        [this](const char *reason) {
            blog(LOG_WARNING, "[Cams] Packet loss detected: %s; requesting keyframe",
                 reason ? reason : "unknown");
            m_controlServer->sendKeyframeRequest();
        }
    );

    if (!m_listener->start(kVideoPort)) {
        blog(LOG_ERROR, "[Cams] NetworkListener failed to start on port %u", kVideoPort);
        return;
    }

    m_listener->startDiscovery();

    // Use an ephemeral port (0) for the control server to prevent port collision
    // with iOS Network.framework when it sets up the two-way NWConnection.
    if (!m_controlServer->start(0)) {
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
    FrameType currentCodec = FrameType::H264;
    while (m_running.load()) {
        auto frameOpt = m_jitterBuffer->pop();
        if (!frameOpt) {
            // Buffer was flushed.  If we're still running (e.g. buffer mode
            // changed), keep looping instead of exiting.
            if (m_running.load()) continue;
            break;
        }

        const auto &frame = *frameOpt;

        // Detect the actual codec from the frame type.
        if (frame.frameType == FrameType::HEVC || frame.frameType == FrameType::H264) {
            currentCodec = frame.frameType;
        } else if (frame.frameType == FrameType::Keyframe) {
            currentCodec = detectCodecFromAnnexB(frame.data, currentCodec);
        } else if (frame.frameType == FrameType::ParameterSets) {
            currentCodec = detectCodecFromAnnexB(frame.data, currentCodec);
        }
        FrameType expectedCodec = currentCodec;

        // Open or reopen the decoder if needed.
        if (!m_decoder->isOpen()) {
            if (!m_decoder->open(expectedCodec)) {
                m_controlServer->sendKeyframeRequest();
                continue;
            }
            m_decoderCodec = expectedCodec;
            blog(LOG_INFO, "[Cams] Decoder opened with codec %s",
                 expectedCodec == FrameType::HEVC ? "HEVC" : "H.264");
        } else if (expectedCodec != m_decoderCodec) {
            // Codec changed mid-stream — reopen the decoder.
            blog(LOG_INFO, "[Cams] Codec changed from %s to %s — reopening decoder",
                 m_decoderCodec == FrameType::HEVC ? "HEVC" : "H.264",
                 expectedCodec == FrameType::HEVC ? "HEVC" : "H.264");
            m_decoder->close();
            if (!m_decoder->open(expectedCodec)) {
                m_controlServer->sendKeyframeRequest();
                continue;
            }
            m_decoderCodec = expectedCodec;
            m_controlServer->sendKeyframeRequest();
            continue; // Skip this frame; wait for a fresh keyframe.
        }

        bool ok = m_decoder->decode(frame.data, frame.frameType, [this](AVFrame *avFrame) {
            outputAVFrame(avFrame);
        });

        if (!ok) {
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

    obs_data_t *settings = obs_source_get_settings(self->m_source);
    bool currentlyLocked = obs_data_get_bool(settings, kSettingFocusLock);
    bool newLocked = !currentlyLocked;

    self->m_controlServer->sendFocusLock(newLocked);
    obs_data_set_bool(settings, kSettingFocusLock, newLocked);
    obs_data_release(settings);
    return true;
}

bool CamsSource::onExposureLockClicked(
    obs_properties_t * /*props*/,
    obs_property_t   * /*prop*/,
    void             *data
) {
    auto *self = static_cast<CamsSource *>(data);
    if (!self) return false;

    obs_data_t *settings = obs_source_get_settings(self->m_source);
    bool currentlyLocked = obs_data_get_bool(settings, kSettingExpLock);
    bool newLocked = !currentlyLocked;

    self->m_controlServer->sendExposureLock(newLocked);
    obs_data_set_bool(settings, kSettingExpLock, newLocked);
    obs_data_release(settings);
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
        std::ostringstream label;
        label << dev.name << " (" << dev.hostName << ":" << dev.port << ")";
        obs_property_list_add_string(
            deviceProp, label.str().c_str(), dev.hostName.c_str());
    }
    return true;
}

bool CamsSource::onActivateClicked(
    obs_properties_t * /*props*/,
    obs_property_t   * /*prop*/,
    void             *data
) {
    auto *self = static_cast<CamsSource *>(data);
    if (!self) return false;

    const std::string targetHost = !self->m_manualHost.empty()
        ? self->m_manualHost
        : self->m_selectedDevice;
    if (targetHost.empty()) {
        blog(LOG_WARNING, "[Cams] Activate requested but no iPhone target is selected");
        return false;
    }

    self->activateTarget();
    blog(LOG_INFO, "[Cams] Activate sent to %s:%u", targetHost.c_str(), kControlPort);
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
    info.icon_type    = OBS_ICON_TYPE_CAMERA;
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

// VideoDecoder.cpp
// Cams OBS Plugin — FFmpeg hardware-accelerated video decoder implementation.

#include "VideoDecoder.h"

#include <array>
#include <cstdio>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
}

namespace cams {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string avError(int errnum) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errnum, buf, sizeof(buf));
    return std::string(buf);
}

// Hardware device preference order per platform.
static std::array<AVHWDeviceType, 4> preferredHWTypes() {
#if defined(__APPLE__)
    return {AV_HWDEVICE_TYPE_VIDEOTOOLBOX, AV_HWDEVICE_TYPE_NONE,
            AV_HWDEVICE_TYPE_NONE,         AV_HWDEVICE_TYPE_NONE};
#elif defined(_WIN32)
    return {AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_CUDA,
            AV_HWDEVICE_TYPE_DXVA2,   AV_HWDEVICE_TYPE_NONE};
#else
    return {AV_HWDEVICE_TYPE_VAAPI,   AV_HWDEVICE_TYPE_CUDA,
            AV_HWDEVICE_TYPE_DRM,     AV_HWDEVICE_TYPE_NONE};
#endif
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

VideoDecoder::VideoDecoder() {}

VideoDecoder::~VideoDecoder() {
    close();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool VideoDecoder::open(FrameType codec) {
    close();

    enum AVCodecID codecId =
        (codec == FrameType::HEVC || codec == FrameType::ParameterSets)
        ? AV_CODEC_ID_HEVC
        : AV_CODEC_ID_H264;

    const AVCodec *avCodec = avcodec_find_decoder(codecId);
    if (!avCodec) {
        fprintf(stderr, "[Cams] avcodec_find_decoder failed for codec %d\n",
                static_cast<int>(codecId));
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(avCodec);
    if (!m_codecCtx) {
        fprintf(stderr, "[Cams] avcodec_alloc_context3 returned null\n");
        return false;
    }

    // Enable low-delay decoding.
    m_codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    av_opt_set_int(m_codecCtx, "threads", 1, 0);  // Fewer threads = lower latency.

    // Attempt hardware acceleration.
    initHardware(m_codecCtx);

    int ret = avcodec_open2(m_codecCtx, avCodec, nullptr);
    if (ret < 0) {
        fprintf(stderr, "[Cams] avcodec_open2 failed: %s\n", avError(ret).c_str());
        close();
        return false;
    }

    m_packet   = av_packet_alloc();
    m_frame    = av_frame_alloc();
    m_swFrame  = av_frame_alloc();

    if (!m_packet || !m_frame || !m_swFrame) {
        fprintf(stderr, "[Cams] Frame/packet allocation failed\n");
        close();
        return false;
    }

    m_needsKeyframe = true;
    return true;
}

void VideoDecoder::close() {
    av_frame_free(&m_swFrame);
    av_frame_free(&m_frame);
    av_packet_free(&m_packet);
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }
    if (m_hwDeviceCtx) {
        av_buffer_unref(&m_hwDeviceCtx);
        m_hwDeviceCtx  = nullptr;
        m_hwDeviceType = AV_HWDEVICE_TYPE_NONE;
    }
    m_needsKeyframe = true;
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

bool VideoDecoder::decode(const std::vector<uint8_t> &data, FrameCallback onFrame) {
    if (!m_codecCtx) return false;

    // If we haven't received a keyframe yet, discard until we do.
    if (m_needsKeyframe) {
        // A basic check: HEVC/H264 IDR NAL starts with specific byte patterns.
        // We rely on the sender marking keyframes with FrameType::Keyframe.
        // The actual IDR detection is handled server-side by the iOS app.
        // Here we simply pass through and wait for the decoder to sync.
    }

    m_packet->data = const_cast<uint8_t *>(data.data());
    m_packet->size = static_cast<int>(data.size());

    int ret = avcodec_send_packet(m_codecCtx, m_packet);
    if (ret < 0) {
        if (ret == AVERROR_INVALIDDATA) {
            // Corrupted or incomplete packet — request a keyframe and continue.
            m_needsKeyframe = true;
            return true;  // Non-fatal; allow recovery on next keyframe.
        }
        fprintf(stderr, "[Cams] avcodec_send_packet error: %s\n", avError(ret).c_str());
        reset();
        return false;
    }

    while (true) {
        ret = avcodec_receive_frame(m_codecCtx, m_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            fprintf(stderr, "[Cams] avcodec_receive_frame error: %s\n", avError(ret).c_str());
            reset();
            return false;
        }

        AVFrame *outFrame = m_frame;

        // If the frame lives on the hardware device, transfer it to RAM.
        if (m_frame->format == m_codecCtx->pix_fmt &&
            m_hwDeviceCtx != nullptr)
        {
            ret = av_hwframe_transfer_data(m_swFrame, m_frame, 0);
            if (ret < 0) {
                fprintf(stderr, "[Cams] av_hwframe_transfer_data failed: %s\n",
                        avError(ret).c_str());
                av_frame_unref(m_frame);
                continue;
            }
            m_swFrame->pts = m_frame->pts;
            outFrame = m_swFrame;
        }

        m_needsKeyframe = false;
        if (onFrame) onFrame(outFrame);
        av_frame_unref(m_frame);
        av_frame_unref(m_swFrame);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Info
// ---------------------------------------------------------------------------

std::string VideoDecoder::hwAccelName() const {
    if (m_hwDeviceType == AV_HWDEVICE_TYPE_NONE) return "software";
    return std::string(av_hwdevice_get_type_name(m_hwDeviceType));
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void VideoDecoder::initHardware(AVCodecContext *ctx) {
    for (AVHWDeviceType type : preferredHWTypes()) {
        if (type == AV_HWDEVICE_TYPE_NONE) break;

        AVBufferRef *deviceCtx = nullptr;
        int ret = av_hwdevice_ctx_create(&deviceCtx, type, nullptr, nullptr, 0);
        if (ret < 0) continue;

        ctx->hw_device_ctx = av_buffer_ref(deviceCtx);
        av_buffer_unref(&deviceCtx);
        m_hwDeviceCtx  = ctx->hw_device_ctx;
        m_hwDeviceType = type;
        return;
    }
    // No hardware acceleration available; proceed with software decode.
}

void VideoDecoder::reset() {
    FrameType lastCodec = (m_codecCtx &&
        m_codecCtx->codec_id == AV_CODEC_ID_HEVC)
        ? FrameType::HEVC : FrameType::H264;
    close();
    open(lastCodec);
}

} // namespace cams

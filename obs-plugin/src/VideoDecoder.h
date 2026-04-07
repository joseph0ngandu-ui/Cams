// VideoDecoder.h
// Cams OBS Plugin — FFmpeg hardware-accelerated video decoder.
//
// Wraps AVCodecContext with automatic hardware device selection:
//   • macOS: VideoToolbox
//   • Windows: D3D11VA / CUDA
//   • Linux: VAAPI / CUDA
//   • Fallback: software decode
//
// Thread safety: `decode()` is NOT thread-safe; call only from the OBS
// video render thread (or any single thread you dedicate to decoding).

#pragma once

#include "CamsPacket.h"
#include "JitterBuffer.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

// Forward-declare the OBS video frame type to avoid pulling in the full OBS
// header from this translation unit.
struct video_data;

namespace cams {

// ---------------------------------------------------------------------------
// VideoDecoder
// ---------------------------------------------------------------------------

/// Decodes encoded video frames via FFmpeg and produces OBS video_data frames.
class VideoDecoder {
public:
    /// Callback invoked on each decoded frame.
    /// `frame` is valid only for the duration of the callback.
    using FrameCallback = std::function<void(AVFrame *frame)>;

    VideoDecoder();
    ~VideoDecoder();

    // Non-copyable / non-movable.
    VideoDecoder(const VideoDecoder &)            = delete;
    VideoDecoder &operator=(const VideoDecoder &) = delete;

    // -----------------------------------------------------------------------
    // Lifecycle

    /// Opens the decoder for `codec`.  Attempts hardware acceleration first.
    /// Returns true on success.  Resets any existing session.
    bool open(FrameType codec);

    /// Closes the decoder and frees all FFmpeg resources.
    void close();

    bool isOpen() const { return m_codecCtx != nullptr; }

    // -----------------------------------------------------------------------
    // Decoding

    /// Submits `data` for decoding.
    ///
    /// On success the `onFrame` callback is invoked for each decoded picture.
    /// Returns false if a fatal error occurs; the decoder is automatically
    /// reset and `open()` must be called again.
    bool decode(const std::vector<uint8_t> &data, FrameCallback onFrame);

    // -----------------------------------------------------------------------
    // Info

    /// Human-readable name of the active hardware accelerator (or "software").
    std::string hwAccelName() const;

private:
    AVCodecContext       *m_codecCtx      = nullptr;
    AVBufferRef          *m_hwDeviceCtx   = nullptr;
    AVPacket             *m_packet        = nullptr;
    AVFrame              *m_frame         = nullptr;
    AVFrame              *m_swFrame       = nullptr;   ///< Software fallback frame.
    enum AVHWDeviceType   m_hwDeviceType  = AV_HWDEVICE_TYPE_NONE;
    bool                  m_needsKeyframe = true;

    // Attempts to initialise the best available hardware device context.
    void initHardware(AVCodecContext *ctx);

    // Resets the decoder after a fatal error.
    void reset();
};

} // namespace cams

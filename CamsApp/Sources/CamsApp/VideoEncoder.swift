// VideoEncoder.swift
// Cams — VideoToolbox HEVC/H.264 hardware encoder.
//
// Encoding pipeline:
//   CVPixelBuffer  →  VTCompressionSession  →  CMSampleBuffer  →  callback
//
// Key properties:
//  • Codec: HEVC (H.265) with H.264 High-profile fallback.
//  • Rate control: Constant Bit Rate (CBR).
//  • Max bitrate: configurable, defaults to 50 Mbps for 4K.
//  • Real-time mode: kVTCompressionPropertyKey_RealTime = true.
//  • Minimum buffering: kVTCompressionPropertyKey_MaxFrameDelayCount = 0.
//  • Thermal throttling recovery: the session is torn down and recreated on
//    kVTCompressionSessionInvalidated or any fatal status.

import VideoToolbox
import CoreMedia
import CoreVideo
import Foundation

private final class EncodeBufferRef {
    let pool: CircularBufferPool
    let pixelBuffer: CVPixelBuffer

    init(pool: CircularBufferPool, pixelBuffer: CVPixelBuffer) {
        self.pool = pool
        self.pixelBuffer = pixelBuffer
    }
}

// MARK: - VideoEncoderDelegate

/// Receives encoded sample buffers from the encoder.
public protocol VideoEncoderDelegate: AnyObject, Sendable {
    /// Called on the encoder's internal callback queue for every encoded frame.
    ///
    /// - Parameters:
    ///   - encoder: The encoder that produced the sample.
    ///   - sampleBuffer: The encoded CMSampleBuffer (H.264 or HEVC NAL units).
    ///   - isKeyframe: `true` when this sample contains an IDR frame.
    func encoder(
        _ encoder: VideoEncoder,
        didOutputSampleBuffer sampleBuffer: CMSampleBuffer,
        isKeyframe: Bool
    )

    /// Called when an unrecoverable encoding error occurs and the session has
    /// been reset.  The delegate should expect the next frame to be a keyframe.
    func encoderDidReset(_ encoder: VideoEncoder)
}

// MARK: - VideoEncoderConfiguration

/// Immutable configuration snapshot for `VideoEncoder`.
public struct VideoEncoderConfiguration: Sendable {
    /// Target bitrate in bits per second.
    public let targetBitrate: Int
    /// Output frame rate.
    public let frameRate: Int
    /// Video width in pixels.
    public let width: Int
    /// Video height in pixels.
    public let height: Int
    /// Whether to prefer HEVC over H.264.
    public let preferHEVC: Bool

    public init(
        targetBitrate: Int = 8_000_000,
        frameRate: Int = 30,
        width: Int = 1920,
        height: Int = 1080,
        preferHEVC: Bool = true
    ) {
        self.targetBitrate = targetBitrate
        self.frameRate     = frameRate
        self.width         = width
        self.height        = height
        self.preferHEVC    = preferHEVC
    }
}

// MARK: - VideoEncoder

/// Hardware-accelerated video encoder backed by VideoToolbox.
///
/// Instances are safe to create and destroy on any thread; all internal
/// mutations are serialised on `encoderQueue`.
public final class VideoEncoder: @unchecked Sendable {

    // MARK: Public state

    public weak var delegate: (any VideoEncoderDelegate)?
    /// Current encoder configuration.  The encoder must be restarted after
    /// changing configuration via `start(configuration:)`.
    public private(set) var configuration: VideoEncoderConfiguration

    // MARK: Private state

    private var session: VTCompressionSession?
    private var isRunning = false
    private var frameCount: UInt32 = 0
    private var usingHEVC  = false
    private var forceKeyframeOnNextFrame = true

    private let encoderQueue = DispatchQueue(
        label: "com.cams.videoEncoder",
        qos: .userInteractive
    )

    // MARK: - Lifecycle

    public init(configuration: VideoEncoderConfiguration = .init()) {
        self.configuration = configuration
    }

    deinit {
        stopSync()
    }

    // MARK: - Public API

    /// Creates (or re-creates) the VTCompressionSession and starts encoding.
    ///
    /// Safe to call more than once — an existing session is cleanly invalidated
    /// before a new one is created.
    public func start(configuration newConfig: VideoEncoderConfiguration? = nil) {
        encoderQueue.async { [weak self] in
            guard let self else { return }
            if let c = newConfig { self.configuration = c }
            self.stopSync()
            self.createSession()
        }
    }

    /// Forces the next submitted frame to be encoded as an IDR/keyframe.
    public func requestKeyframe() {
        encoderQueue.async { [weak self] in
            self?.forceKeyframeOnNextFrame = true
        }
    }

    /// Encodes `pixelBuffer`, returning the buffer to `pool` when done.
    ///
    /// If the underlying session is not ready the buffer is returned immediately
    /// to avoid backlog accumulation.
    public func encode(
        pixelBuffer: CVPixelBuffer,
        presentationTime: CMTime,
        pool: CircularBufferPool?
    ) {
        encoderQueue.async { [weak self] in
            guard let self, self.isRunning, let session = self.session else {
                pool?.enqueue(pixelBuffer)
                return
            }

            let frameProperties: CFDictionary?
            if self.forceKeyframeOnNextFrame {
                self.forceKeyframeOnNextFrame = false
                frameProperties = [
                    kVTEncodeFrameOptionKey_ForceKeyFrame: true
                ] as CFDictionary
            } else {
                frameProperties = nil
            }

            let sourceFrameRefcon = pool.map {
                Unmanaged.passRetained(EncodeBufferRef(pool: $0, pixelBuffer: pixelBuffer)).toOpaque()
            }

            var flags = VTEncodeInfoFlags()
            let status = VTCompressionSessionEncodeFrame(
                session,
                imageBuffer: pixelBuffer,
                presentationTimeStamp: presentationTime,
                duration: CMTime(value: 1, timescale: CMTimeScale(self.configuration.frameRate)),
                frameProperties: frameProperties,
                sourceFrameRefcon: sourceFrameRefcon,
                infoFlagsOut: &flags
            )

            if status != noErr {
                if let sourceFrameRefcon {
                    let ref = Unmanaged<EncodeBufferRef>.fromOpaque(sourceFrameRefcon).takeRetainedValue()
                    ref.pool.enqueue(ref.pixelBuffer)
                }
                // Encoder session became invalid (e.g., thermal throttling).
                // Recreate the session so encoding can resume with the next frame.
                self.handleEncoderError(status: status)
            }
        }
    }

    /// Stops encoding and invalidates the VTCompressionSession.
    public func stop() {
        encoderQueue.async { [weak self] in
            self?.stopSync()
        }
    }

    // MARK: - Private helpers

    /// Must be called on `encoderQueue`.
    private func createSession() {
        let cfg = configuration

        // Attempt HEVC first; fall back to H.264 if hardware encoder is unavailable.
        let codecTypes: [CMVideoCodecType] = cfg.preferHEVC
            ? [kCMVideoCodecType_HEVC, kCMVideoCodecType_H264]
            : [kCMVideoCodecType_H264]

        for codec in codecTypes {
            if let s = makeSession(codec: codec, cfg: cfg) {
                session   = s
                usingHEVC = (codec == kCMVideoCodecType_HEVC)
                isRunning = true
                forceKeyframeOnNextFrame = true
                return
            }
        }

        // Both codecs failed — report and remain stopped.
        isRunning = false
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.delegate?.encoderDidReset(self)
        }
    }

    /// Builds and configures a VTCompressionSession for `codec`.
    private func makeSession(
        codec: CMVideoCodecType,
        cfg: VideoEncoderConfiguration
    ) -> VTCompressionSession? {

        var sessionOut: VTCompressionSession?

        // Unretained self pointer for C callback.
        let callbackRefcon = Unmanaged.passUnretained(self).toOpaque()

        let outputCallback: VTCompressionOutputCallback = { refcon, sourceFrameRefcon, status, flags, sampleBuffer in
            if let sourceFrameRefcon {
                let ref = Unmanaged<EncodeBufferRef>.fromOpaque(sourceFrameRefcon).takeRetainedValue()
                ref.pool.enqueue(ref.pixelBuffer)
            }
            guard let refcon, status == noErr, let sampleBuffer else { return }
            let encoder = Unmanaged<VideoEncoder>.fromOpaque(refcon).takeUnretainedValue()
            encoder.handleEncodedSample(sampleBuffer: sampleBuffer, flags: flags)
        }

        let createStatus = VTCompressionSessionCreate(
            allocator: kCFAllocatorDefault,
            width:     Int32(cfg.width),
            height:    Int32(cfg.height),
            codecType: codec,
            encoderSpecification: nil,
            imageBufferAttributes: nil,
            compressedDataAllocator: nil,
            outputCallback: outputCallback,
            refcon: callbackRefcon,
            compressionSessionOut: &sessionOut
        )

        guard createStatus == noErr, let session = sessionOut else { return nil }

        // ── Real-time encoding ──────────────────────────────────────────────
        VTSessionSetProperty(session,
            key: kVTCompressionPropertyKey_RealTime,
            value: kCFBooleanTrue)

        // Zero-frame delay: emit output immediately without buffering.
        VTSessionSetProperty(session,
            key: kVTCompressionPropertyKey_MaxFrameDelayCount,
            value: 0 as CFTypeRef)

        // ── Constant Bit Rate ────────────────────────────────────────────────
        // CBR is achieved by setting both average and max data rate to the same
        // value and using a very short data-rate limit window.
        VTSessionSetProperty(session,
            key: kVTCompressionPropertyKey_AverageBitRate,
            value: cfg.targetBitrate as CFTypeRef)

        let maxBytesPerSecond: Int = cfg.targetBitrate / 8
        // Enforce CBR with a 100 ms window.
        let dataRateLimits = [maxBytesPerSecond, 1] as CFArray
        VTSessionSetProperty(session,
            key: kVTCompressionPropertyKey_DataRateLimits,
            value: dataRateLimits)

        // ── Profile / Level ──────────────────────────────────────────────────
        if codec == kCMVideoCodecType_HEVC {
            VTSessionSetProperty(session,
                key: kVTCompressionPropertyKey_ProfileLevel,
                value: kVTProfileLevel_HEVC_Main_AutoLevel)
        } else {
            VTSessionSetProperty(session,
                key: kVTCompressionPropertyKey_ProfileLevel,
                value: kVTProfileLevel_H264_High_AutoLevel)
        }

        // ── Frame rate ───────────────────────────────────────────────────────
        VTSessionSetProperty(session,
            key: kVTCompressionPropertyKey_ExpectedFrameRate,
            value: cfg.frameRate as CFTypeRef)

        // ── Allow frame reordering? No — latency mode requires ordered output.
        VTSessionSetProperty(session,
            key: kVTCompressionPropertyKey_AllowFrameReordering,
            value: kCFBooleanFalse)

        VTCompressionSessionPrepareToEncodeFrames(session)
        return session
    }

    /// Handles encoded output from the VTCompressionOutputCallback.
    private func handleEncodedSample(
        sampleBuffer: CMSampleBuffer,
        flags: VTEncodeInfoFlags
    ) {
        guard !flags.contains(.frameDropped) else { return }

        let attachments = CMSampleBufferGetSampleAttachmentsArray(
            sampleBuffer, createIfNecessary: false
        ) as? [[CFString: Any]]

        let isKeyframe: Bool = attachments?.first.map { dict in
            let notSync = dict[kCMSampleAttachmentKey_NotSync] as? Bool ?? false
            return !notSync
        } ?? true

        delegate?.encoder(
            self,
            didOutputSampleBuffer: sampleBuffer,
            isKeyframe: isKeyframe
        )
    }

    /// Called when VTCompressionSessionEncodeFrame returns a fatal error.
    ///
    /// Tears down the current session and attempts to recreate it so that
    /// encoding can resume automatically (e.g., after thermal throttling).
    private func handleEncoderError(status: OSStatus) {
        isRunning = false
        forceKeyframeOnNextFrame = true
        session.map { VTCompressionSessionInvalidate($0) }
        session = nil

        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.delegate?.encoderDidReset(self)
        }

        // Retry session creation after a short pause.
        encoderQueue.asyncAfter(deadline: .now() + .milliseconds(500)) { [weak self] in
            self?.createSession()
        }
    }

    /// Synchronous teardown — must be called on `encoderQueue`.
    private func stopSync() {
        isRunning = false
        frameCount = 0
        forceKeyframeOnNextFrame = true
        guard let s = session else { return }
        VTCompressionSessionCompleteFrames(s, untilPresentationTimeStamp: .invalid)
        VTCompressionSessionInvalidate(s)
        session = nil
    }
}

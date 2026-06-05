// CenterStageEngine.swift
// Cams — Subject tracking via hardware Center Stage or Vision framework software fallback.
//
// Hardware path (iPad with Ultra Wide front camera):
//   Toggles AVCaptureDevice.isCenterStageEnabled directly — zero CPU cost.
//
// Software path (all iPhones, unsupported iPads):
//   Runs VNDetectHumanRectanglesRequest on a dedicated vision queue.
//   Applies exponential smoothing (α=0.15) to the bounding box so the
//   digital crop feels like a weighted cinematic glide rather than snapping.
//   The smoothed crop is applied via CIContext on every captured frame
//   without ever blocking the captureQueue.

import AVFoundation
import CoreImage
import CoreVideo
import Vision
import os.lock

// MARK: - CenterStageEngine

public final class CenterStageEngine: @unchecked Sendable {

    // MARK: - Public state

    /// Whether hardware Center Stage is available for the current device.
    public let usesHardwarePath: Bool

    /// Current enabled state (hardware and software paths).
    public private(set) var isEnabled: Bool = false

    // MARK: - Private state

    // All mutable state below is accessed exclusively on visionQueue, except
    // _visionInFlight which is read on captureQueue and written on visionQueue;
    // it is protected by OSAllocatedUnfairLock for Sendable-safe atomic access.
    private let visionQueue = DispatchQueue(
        label: "com.cams.centerStage.vision",
        qos: .userInitiated
    )

    private let ciContext = CIContext(options: [.useSoftwareRenderer: false])
    private var lastSmoothedRect: CGRect?

    // OSAllocatedUnfairLock is available iOS 16+ (matching deployment target).
    private let visionInFlightLock = OSAllocatedUnfairLock(initialState: false)

    private var outputBufferPool: CVPixelBufferPool?
    private var encoderWidth: Int = 1920
    private var encoderHeight: Int = 1080

    // MARK: - Init

    public init() {
        #if os(iOS)
        usesHardwarePath = AVCaptureDevice.isCenterStageSupported
        #else
        usesHardwarePath = false
        #endif
    }

    // MARK: - Configuration

    /// Call before `startCapture`. Allocates the output pixel buffer pool for
    /// the software path and configures hardware Center Stage if supported.
    public func configure(encoderWidth: Int, encoderHeight: Int) {
        self.encoderWidth  = encoderWidth
        self.encoderHeight = encoderHeight

        guard !usesHardwarePath else { return }
        createOutputPool(width: encoderWidth, height: encoderHeight)
    }

    // MARK: - Enable / Disable

    /// Toggles subject tracking. On hardware path this calls the device-wide
    /// `AVCaptureDevice.isCenterStageEnabled` class property. On software path
    /// the engine processes frames only when `isEnabled == true`.
    public func setEnabled(_ enabled: Bool) {
        isEnabled = enabled
        if usesHardwarePath {
            #if os(iOS)
            AVCaptureDevice.isCenterStageEnabled = enabled
            #endif
        }
        if !enabled {
            // Reset smoothed rect so the crop immediately goes full-frame when
            // re-enabled, then glides in from there.
            visionQueue.async { [weak self] in
                self?.lastSmoothedRect = nil
            }
        }
    }

    // MARK: - Frame Processing

    /// Routes a `CVPixelBuffer` through the Center Stage pipeline.
    ///
    /// - Returns: The original buffer when hardware tracking is active or the
    ///   engine is disabled. Returns a cropped-and-scaled buffer from the pool
    ///   on the software path.
    ///
    /// - Important: This method is called on `captureQueue` and **must not
    ///   block**. Vision requests run asynchronously on `visionQueue`.
    public func process(pixelBuffer: CVPixelBuffer) -> CVPixelBuffer {
        guard isEnabled, !usesHardwarePath else { return pixelBuffer }

        // Kick off an async Vision request if one is not already running.
        let alreadyInFlight = visionInFlightLock.withLock { state -> Bool in
            if state { return true }
            state = true
            return false
        }
        if !alreadyInFlight {
            // Capturing `pixelBuffer` in the closure increments its ARC retain count,
            // keeping it alive until the async block completes on visionQueue.
            visionQueue.async { [weak self, pixelBuffer] in
                guard let self else { return }
                self.runVision(on: pixelBuffer)
                self.visionInFlightLock.withLock { $0 = false }
            }
        }

        // Apply the latest smoothed crop rectangle (stale reads are fine here;
        // the worst outcome is applying the previous frame's crop).
        guard let cropRect = lastSmoothedRect,
              cropRect != CGRect(x: 0, y: 0, width: 1, height: 1) else {
            return pixelBuffer
        }

        return cropAndScale(pixelBuffer, normalizedCrop: cropRect) ?? pixelBuffer
    }

    // MARK: - Vision Detection

    private func runVision(on pixelBuffer: CVPixelBuffer) {
        let request = VNDetectHumanRectanglesRequest()
        request.upperBodyOnly = false

        let handler = VNImageRequestHandler(cvPixelBuffer: pixelBuffer, options: [:])
        try? handler.perform([request])

        guard let results = request.results, !results.isEmpty else { return }

        let imageWidth  = CGFloat(CVPixelBufferGetWidth(pixelBuffer))
        let imageHeight = CGFloat(CVPixelBufferGetHeight(pixelBuffer))
        updateSmoothedRect(from: results, imageSize: CGSize(width: imageWidth, height: imageHeight))
    }

    // MARK: - Bounding Box + Smoothing

    private func updateSmoothedRect(from observations: [VNHumanObservation], imageSize: CGSize) {
        // VN coordinates have origin at bottom-left; convert to top-left.
        var union = observations[0].boundingBox
        for obs in observations.dropFirst() {
            union = union.union(obs.boundingBox)
        }

        let target = CGRect(
            x:      union.origin.x,
            y:      1.0 - union.origin.y - union.size.height,
            width:  union.size.width,
            height: union.size.height
        )

        // Add 40 % padding uniformly so subjects have breathing room.
        let padX = target.width  * 0.20
        let padY = target.height * 0.20
        var padded = CGRect(
            x:      target.origin.x - padX,
            y:      target.origin.y - padY,
            width:  target.width  + padX * 2,
            height: target.height + padY * 2
        )
        // Clamp to [0, 1].
        let full = CGRect(x: 0, y: 0, width: 1, height: 1)
        padded = padded.intersection(full)

        // Exponential smoothing: α = 0.15 gives a weighted cinematic glide.
        let alpha: CGFloat = 0.15
        if let prev = lastSmoothedRect {
            lastSmoothedRect = CGRect(
                x:      alpha * padded.origin.x + (1 - alpha) * prev.origin.x,
                y:      alpha * padded.origin.y + (1 - alpha) * prev.origin.y,
                width:  alpha * padded.width    + (1 - alpha) * prev.width,
                height: alpha * padded.height   + (1 - alpha) * prev.height
            )
        } else {
            lastSmoothedRect = padded
        }
    }

    // MARK: - Crop + Scale

    private func cropAndScale(
        _ pixelBuffer: CVPixelBuffer,
        normalizedCrop rect: CGRect
    ) -> CVPixelBuffer? {
        guard let pool = outputBufferPool else { return nil }

        let inputW = CGFloat(CVPixelBufferGetWidth(pixelBuffer))
        let inputH = CGFloat(CVPixelBufferGetHeight(pixelBuffer))

        let pixelCrop = CGRect(
            x:      rect.origin.x * inputW,
            y:      rect.origin.y * inputH,
            width:  rect.width    * inputW,
            height: rect.height   * inputH
        )

        guard pixelCrop.width > 0, pixelCrop.height > 0 else { return nil }

        let scaleX = CGFloat(encoderWidth)  / pixelCrop.width
        let scaleY = CGFloat(encoderHeight) / pixelCrop.height

        // CIImage origin is bottom-left; AVFoundation pixel buffers have origin
        // at top-left, so we flip the Y axis when computing the crop rect.
        let flippedY = inputH - pixelCrop.origin.y - pixelCrop.height
        let ciCropRect = CGRect(x: pixelCrop.origin.x, y: flippedY,
                                 width: pixelCrop.width, height: pixelCrop.height)

        let cropped = CIImage(cvPixelBuffer: pixelBuffer)
            .cropped(to: ciCropRect)
            .transformed(by: CGAffineTransform(
                translationX: -ciCropRect.origin.x,
                y: -ciCropRect.origin.y
            ))
            .transformed(by: CGAffineTransform(scaleX: scaleX, y: scaleY))

        var outBuffer: CVPixelBuffer?
        guard CVPixelBufferPoolCreatePixelBuffer(nil, pool, &outBuffer) == kCVReturnSuccess,
              let output = outBuffer
        else { return nil }

        ciContext.render(cropped, to: output)
        return output
    }

    // MARK: - Buffer Pool

    private func createOutputPool(width: Int, height: Int) {
        let attrs: [String: Any] = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            kCVPixelBufferWidthKey           as String: width,
            kCVPixelBufferHeightKey          as String: height,
            kCVPixelBufferIOSurfacePropertiesKey as String: [:]
        ]
        var pool: CVPixelBufferPool?
        CVPixelBufferPoolCreate(nil, nil, attrs as CFDictionary, &pool)
        outputBufferPool = pool
    }
}

// CaptureSession.swift
// Cams — AVFoundation camera capture pipeline.
//
// Pipeline:
//   AVCaptureDevice  →  AVCaptureVideoDataOutput  →  VideoEncoder  →  NetworkTransport
//
// Key design decisions:
//  • Uses `.userInteractive` DispatchQueue for the sample buffer callback to
//    minimise latency on the capture path.
//  • Feeds CVPixelBuffers directly to VTCompressionSession via the encoder
//    without any intermediate conversion.
//  • On thermal throttling (ThermalStateSerious/Critical) the encoder bitrate
//    is automatically halved to reduce heat generation.

import AVFoundation
import CoreMedia
import CoreVideo
import Foundation

// MARK: - CaptureSessionDelegate

public protocol CaptureSessionDelegate: AnyObject, Sendable {
    /// Called when the capture session starts or stops.
    func captureSession(_ session: CaptureSession, isRunning: Bool)
    /// Called when an unrecoverable setup error occurs.
    func captureSession(_ session: CaptureSession, didFailWithError error: Error)
}

// MARK: - CaptureSession

/// Manages the AVCaptureSession pipeline for real-time camera capture.
public final class CaptureSession: NSObject, @unchecked Sendable {

    // MARK: Public state

    public weak var delegate: (any CaptureSessionDelegate)?
    public let encoder: VideoEncoder
    public let transport: NetworkTransport

    // MARK: Private state

    private let avSession = AVCaptureSession()
    private let bufferPool: CircularBufferPool?

    // .userInteractive ensures the callback executes at maximum priority.
    private let captureQueue = DispatchQueue(
        label: "com.cams.capture",
        qos: .userInteractive
    )

    private var thermalObserver: Any?
    private var currentConfig: VideoEncoderConfiguration

    // MARK: - Lifecycle

    /// - Parameters:
    ///   - encoderConfig: Initial encoder configuration.
    ///   - transport: The network transport to send encoded data over.
    public init(
        encoderConfig: VideoEncoderConfiguration = .init(),
        transport: NetworkTransport
    ) {
        self.currentConfig = encoderConfig
        self.encoder       = VideoEncoder(configuration: encoderConfig)
        self.transport     = transport

        // Pre-allocate a small pool of reusable pixel buffers.
        self.bufferPool = try? CircularBufferPool(
            capacity: 6,
            width:    encoderConfig.width,
            height:   encoderConfig.height
        )
        super.init()
        encoder.delegate = self
        transport.delegate = self
    }

    deinit {
        stopCapture()
        if let obs = thermalObserver {
            NotificationCenter.default.removeObserver(obs)
        }
    }

    // MARK: - Public API

    /// Configures and starts the capture session for `device`.
    ///
    /// This method configures the `AVCaptureSession` on the calling thread then
    /// starts capture.  Call from a background thread to avoid blocking the UI.
    ///
    /// - Parameter device: The `AVCaptureDevice` to use.  Defaults to the
    ///   built-in wide-angle camera.
    public func startCapture(device: AVCaptureDevice? = nil) throws {
        let captureDevice: AVCaptureDevice
        if let d = device {
            captureDevice = d
        } else if let d = AVCaptureDevice.default(
            .builtInWideAngleCamera,
            for: .video,
            position: .back
        ) {
            captureDevice = d
        } else {
            throw CaptureSessionError.noCameraAvailable
        }

        let input = try AVCaptureDeviceInput(device: captureDevice)

        avSession.beginConfiguration()

        if avSession.canSetSessionPreset(.hd4K3840x2160) {
            avSession.sessionPreset = .hd4K3840x2160
        } else if avSession.canSetSessionPreset(.hd1920x1080) {
            avSession.sessionPreset = .hd1920x1080
        }

        guard avSession.canAddInput(input) else {
            avSession.commitConfiguration()
            throw CaptureSessionError.cannotAddInput
        }
        avSession.addInput(input)

        let output = AVCaptureVideoDataOutput()
        output.videoSettings = [
            kCVPixelBufferPixelFormatTypeKey as String:
                kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        ]
        output.alwaysDiscardsLateVideoFrames = false
        output.setSampleBufferDelegate(self, queue: captureQueue)

        guard avSession.canAddOutput(output) else {
            avSession.commitConfiguration()
            throw CaptureSessionError.cannotAddOutput
        }
        avSession.addOutput(output)

        avSession.commitConfiguration()

        encoder.start()
        avSession.startRunning()

        observeThermalState()

        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.delegate?.captureSession(self, isRunning: true)
        }
    }

    /// Stops capturing and encoding.
    public func stopCapture() {
        avSession.stopRunning()
        encoder.stop()
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.delegate?.captureSession(self, isRunning: false)
        }
    }

    // MARK: - Thermal state management

    private func observeThermalState() {
        thermalObserver = NotificationCenter.default.addObserver(
            forName: ProcessInfo.thermalStateDidChangeNotification,
            object: nil,
            queue: nil
        ) { [weak self] _ in
            self?.handleThermalStateChange()
        }
        handleThermalStateChange()
    }

    private func handleThermalStateChange() {
        let state = ProcessInfo.processInfo.thermalState
        var newBitrate = currentConfig.targetBitrate

        switch state {
        case .nominal, .fair:
            newBitrate = currentConfig.targetBitrate
        case .serious:
            // Halve bitrate to reduce heat.
            newBitrate = currentConfig.targetBitrate / 2
        case .critical:
            // Minimum viable bitrate — 25% of nominal.
            newBitrate = currentConfig.targetBitrate / 4
        @unknown default:
            break
        }

        let reducedConfig = VideoEncoderConfiguration(
            targetBitrate: newBitrate,
            frameRate:     currentConfig.frameRate,
            width:         currentConfig.width,
            height:        currentConfig.height,
            preferHEVC:    currentConfig.preferHEVC
        )
        encoder.start(configuration: reducedConfig)
    }
}

// MARK: - AVCaptureVideoDataOutputSampleBufferDelegate

extension CaptureSession: AVCaptureVideoDataOutputSampleBufferDelegate {

    public func captureOutput(
        _ output: AVCaptureOutput,
        didOutput sampleBuffer: CMSampleBuffer,
        from connection: AVCaptureConnection
    ) {
        guard let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        // Encode directly; the encoder returns the buffer to the pool when done.
        encoder.encode(pixelBuffer: pixelBuffer, presentationTime: pts, pool: nil)
    }

    public func captureOutput(
        _ output: AVCaptureOutput,
        didDrop sampleBuffer: CMSampleBuffer,
        from connection: AVCaptureConnection
    ) {
        // Frame was dropped by the capture pipeline — no action needed;
        // the frame simply won't be encoded or sent.
    }
}

// MARK: - VideoEncoderDelegate

extension CaptureSession: VideoEncoderDelegate {

    public func encoder(
        _ encoder: VideoEncoder,
        didOutputSampleBuffer sampleBuffer: CMSampleBuffer,
        isKeyframe: Bool
    ) {
        guard let dataBuffer = CMSampleBufferGetDataBuffer(sampleBuffer) else { return }

        var totalLength = 0
        var dataPointer: UnsafeMutablePointer<CChar>?
        let status = CMBlockBufferGetDataPointer(
            dataBuffer,
            atOffset: 0,
            lengthAtOffsetOut: nil,
            totalLengthOut: &totalLength,
            dataPointerOut: &dataPointer
        )
        guard status == kCMBlockBufferNoErr, let ptr = dataPointer, totalLength > 0 else { return }

        let data = Data(bytes: ptr, count: totalLength)
        let frameType: FrameType = isKeyframe ? .keyframe : .h264
        transport.send(data: data, frameType: frameType)
    }

    public func encoderDidReset(_ encoder: VideoEncoder) {
        // Encoder was reset (e.g., thermal throttling) — restart it.
        encoder.start(configuration: currentConfig)
    }
}

// MARK: - NetworkTransportDelegate

extension CaptureSession: NetworkTransportDelegate {

    public func transportDidConnect(_ transport: NetworkTransport) {}

    public func transportDidDisconnect(_ transport: NetworkTransport, error: Error?) {}

    public func transportNeedsBackpressure(
        _ transport: NetworkTransport,
        queueDepth: Int
    ) {
        // Network buffer is full — reduce bitrate to ease congestion.
        let reducedBitrate = max(
            currentConfig.targetBitrate / 2,
            1_000_000  // Floor: 1 Mbps
        )
        let reducedConfig = VideoEncoderConfiguration(
            targetBitrate: reducedBitrate,
            frameRate:     currentConfig.frameRate,
            width:         currentConfig.width,
            height:        currentConfig.height,
            preferHEVC:    currentConfig.preferHEVC
        )
        encoder.start(configuration: reducedConfig)
    }
}

// MARK: - CaptureSessionError

public enum CaptureSessionError: Error, LocalizedError {
    case noCameraAvailable
    case cannotAddInput
    case cannotAddOutput

    public var errorDescription: String? {
        switch self {
        case .noCameraAvailable: return "No camera device available on this hardware."
        case .cannotAddInput:    return "Cannot add camera input to capture session."
        case .cannotAddOutput:   return "Cannot add video output to capture session."
        }
    }
}

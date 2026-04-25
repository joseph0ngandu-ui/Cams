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
    /// Called when the UDP transport becomes ready.
    func captureSessionDidConnectTransport(_ session: CaptureSession)
    /// Called when the UDP transport disconnects or waits.
    func captureSession(_ session: CaptureSession, didDisconnectTransport error: Error?)
    /// Called when network congestion forces the encoder to reduce bitrate.
    func captureSession(_ session: CaptureSession, didApplyBackpressureBitrate bitrate: Int)
}

// MARK: - CaptureSession

/// Manages the AVCaptureSession pipeline for real-time camera capture.
public final class CaptureSession: NSObject, @unchecked Sendable {

    // MARK: Public state

    public weak var delegate: (any CaptureSessionDelegate)?
    public let encoder: VideoEncoder
    public let transport: NetworkTransport
    public var previewSession: AVCaptureSession { avSession }
    public private(set) weak var captureDevice: AVCaptureDevice?

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
    private var currentThermalBitrate: Int

    // MARK: - Lifecycle

    /// - Parameters:
    ///   - encoderConfig: Initial encoder configuration.
    ///   - transport: The network transport to send encoded data over.
    public init(
        encoderConfig: VideoEncoderConfiguration = .init(),
        transport: NetworkTransport
    ) {
        self.currentConfig = encoderConfig
        self.currentThermalBitrate = encoderConfig.targetBitrate
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
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized:
            break
        case .notDetermined:
            let semaphore = DispatchSemaphore(value: 0)
            var granted = false
            AVCaptureDevice.requestAccess(for: .video) { ok in
                granted = ok
                semaphore.signal()
            }
            semaphore.wait()
            guard granted else { throw CaptureSessionError.cameraPermissionDenied }
        default:
            throw CaptureSessionError.cameraPermissionDenied
        }

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
        self.captureDevice = captureDevice
        try? configureFrameRate(for: captureDevice)

        let input = try AVCaptureDeviceInput(device: captureDevice)

        avSession.beginConfiguration()

        for oldInput in avSession.inputs {
            avSession.removeInput(oldInput)
        }
        for oldOutput in avSession.outputs {
            avSession.removeOutput(oldOutput)
        }

        applySessionPreset(for: currentConfig)

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

        // Lock the video orientation to portrait so the feed doesn't rotate
        // when the user tilts the device.  Without this, the stream dimensions
        // change mid-stream and the OBS decoder has to constantly reconfigure.
        if let videoConnection = output.connection(with: .video) {
            if videoConnection.isVideoOrientationSupported {
                videoConnection.videoOrientation = .landscapeRight
            }
            // Front camera is mirrored by default; undo that for streaming.
            if videoConnection.isVideoMirroringSupported {
                videoConnection.isVideoMirrored = false
            }
        }

        avSession.commitConfiguration()

        encoder.start(configuration: currentConfig)
        encoder.requestKeyframe()
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
        removeThermalObserver()
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.delegate?.captureSession(self, isRunning: false)
        }
    }

    public func updateEncoderConfiguration(_ config: VideoEncoderConfiguration) {
        currentConfig = config
        currentThermalBitrate = config.targetBitrate
        captureQueue.async { [weak self] in
            guard let self else { return }
            self.avSession.beginConfiguration()
            self.applySessionPreset(for: config)
            self.avSession.commitConfiguration()
            if let device = self.captureDevice {
                try? self.configureFrameRate(for: device)
            }
            self.encoder.start(configuration: config)
            self.encoder.requestKeyframe()
        }
    }

    public func requestKeyframe() {
        encoder.requestKeyframe()
    }

    // MARK: - Thermal state management

    private func observeThermalState() {
        removeThermalObserver()
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

        guard newBitrate != currentThermalBitrate else { return }
        currentThermalBitrate = newBitrate

        let reducedConfig = VideoEncoderConfiguration(
            targetBitrate: newBitrate,
            frameRate:     currentConfig.frameRate,
            width:         currentConfig.width,
            height:        currentConfig.height,
            preferHEVC:    currentConfig.preferHEVC
        )
        encoder.start(configuration: reducedConfig)
        encoder.requestKeyframe()
    }

    private func removeThermalObserver() {
        if let obs = thermalObserver {
            NotificationCenter.default.removeObserver(obs)
            thermalObserver = nil
        }
    }

    private func applySessionPreset(for config: VideoEncoderConfiguration) {
        if config.width >= 3840 && avSession.canSetSessionPreset(.hd4K3840x2160) {
            avSession.sessionPreset = .hd4K3840x2160
        } else if config.width >= 1920 && avSession.canSetSessionPreset(.hd1920x1080) {
            avSession.sessionPreset = .hd1920x1080
        } else if avSession.canSetSessionPreset(.hd1280x720) {
            avSession.sessionPreset = .hd1280x720
        }
    }

    private func configureFrameRate(for device: AVCaptureDevice) throws {
        let fps = Double(currentConfig.frameRate)
        guard device.activeFormat.videoSupportedFrameRateRanges.contains(where: { $0.minFrameRate <= fps && fps <= $0.maxFrameRate }) else {
            return
        }

        try device.lockForConfiguration()
        defer { device.unlockForConfiguration() }
        let duration = CMTime(value: 1, timescale: CMTimeScale(currentConfig.frameRate))
        device.activeVideoMinFrameDuration = duration
        device.activeVideoMaxFrameDuration = duration
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
        if isKeyframe,
           let format = CMSampleBufferGetFormatDescription(sampleBuffer)
        {
            let subType = CMFormatDescriptionGetMediaSubType(format)
            var parameterSets: Data?

            if subType == kCMVideoCodecType_H264 {
                parameterSets = h264ParameterSets(from: format)
            } else if subType == kCMVideoCodecType_HEVC {
                parameterSets = hevcParameterSets(from: format)
            }

            if let parameterSets {
                transport.send(data: parameterSets, frameType: .parameterSets)
            }
        }

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

        let avccData = Data(bytes: ptr, count: totalLength)
        let data = avccToAnnexB(avccData)
        guard !data.isEmpty else { return }

        let format = CMSampleBufferGetFormatDescription(sampleBuffer)
        let subType = format.map { CMFormatDescriptionGetMediaSubType($0) }
        let isHEVC = (subType == kCMVideoCodecType_HEVC)

        let frameType: FrameType
        if isKeyframe {
            frameType = .keyframe
        } else {
            frameType = isHEVC ? .hevc : .h264
        }

        transport.send(data: data, frameType: frameType)
    }

    public func encoderDidReset(_ encoder: VideoEncoder) {
        encoder.requestKeyframe()
    }
}

// MARK: - H.264 helpers

private func avccToAnnexB(_ avcc: Data) -> Data {
    var out = Data()
    var offset = 0
    let lengthFieldSize = 4

    while offset + lengthFieldSize <= avcc.count {
        let naluLength = avcc[offset..<offset + lengthFieldSize].reduce(0) { ($0 << 8) | UInt32($1) }
        offset += lengthFieldSize
        guard naluLength > 0, offset + Int(naluLength) <= avcc.count else {
            return Data()
        }
        out.append(contentsOf: [0x00, 0x00, 0x00, 0x01])
        out.append(avcc[offset..<offset + Int(naluLength)])
        offset += Int(naluLength)
    }

    return out
}

private func h264ParameterSets(from format: CMFormatDescription) -> Data? {
    guard CMFormatDescriptionGetMediaSubType(format) == kCMVideoCodecType_H264 else {
        return nil
    }

    var spsPointer: UnsafePointer<UInt8>?
    var spsSize = 0
    var spsCount = 0
    var naluHeaderLength: Int32 = 0
    var ppsPointer: UnsafePointer<UInt8>?
    var ppsSize = 0
    var ppsCount = 0

    let spsStatus = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
        format,
        parameterSetIndex: 0,
        parameterSetPointerOut: &spsPointer,
        parameterSetSizeOut: &spsSize,
        parameterSetCountOut: &spsCount,
        nalUnitHeaderLengthOut: &naluHeaderLength
    )
    guard spsStatus == noErr, let spsPointer, spsSize > 0 else { return nil }

    let ppsStatus = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
        format,
        parameterSetIndex: 1,
        parameterSetPointerOut: &ppsPointer,
        parameterSetSizeOut: &ppsSize,
        parameterSetCountOut: &ppsCount,
        nalUnitHeaderLengthOut: &naluHeaderLength
    )
    guard ppsStatus == noErr, let ppsPointer, ppsSize > 0 else { return nil }

    var out = Data()
    out.append(contentsOf: [0x00, 0x00, 0x00, 0x01])
    out.append(spsPointer, count: spsSize)
    out.append(contentsOf: [0x00, 0x00, 0x00, 0x01])
    out.append(ppsPointer, count: ppsSize)
    return out
}

private func hevcParameterSets(from format: CMFormatDescription) -> Data? {
    guard CMFormatDescriptionGetMediaSubType(format) == kCMVideoCodecType_HEVC else {
        return nil
    }

    var vpsPointer: UnsafePointer<UInt8>?
    var vpsSize = 0
    var vpsCount = 0
    var naluHeaderLength: Int32 = 0
    var spsPointer: UnsafePointer<UInt8>?
    var spsSize = 0
    var spsCount = 0
    var ppsPointer: UnsafePointer<UInt8>?
    var ppsSize = 0
    var ppsCount = 0

    // HEVC has 3 sets: VPS (0), SPS (1), PPS (2)
    let vpsStatus = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
        format, parameterSetIndex: 0, parameterSetPointerOut: &vpsPointer,
        parameterSetSizeOut: &vpsSize, parameterSetCountOut: &vpsCount, nalUnitHeaderLengthOut: &naluHeaderLength
    )
    guard vpsStatus == noErr, let vpsPointer, vpsSize > 0 else { return nil }

    let spsStatus = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
        format, parameterSetIndex: 1, parameterSetPointerOut: &spsPointer,
        parameterSetSizeOut: &spsSize, parameterSetCountOut: &spsCount, nalUnitHeaderLengthOut: &naluHeaderLength
    )
    guard spsStatus == noErr, let spsPointer, spsSize > 0 else { return nil }

    let ppsStatus = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
        format, parameterSetIndex: 2, parameterSetPointerOut: &ppsPointer,
        parameterSetSizeOut: &ppsSize, parameterSetCountOut: &ppsCount, nalUnitHeaderLengthOut: &naluHeaderLength
    )
    guard ppsStatus == noErr, let ppsPointer, ppsSize > 0 else { return nil }

    var out = Data()
    out.append(contentsOf: [0x00, 0x00, 0x00, 0x01])
    out.append(vpsPointer, count: vpsSize)
    out.append(contentsOf: [0x00, 0x00, 0x00, 0x01])
    out.append(spsPointer, count: spsSize)
    out.append(contentsOf: [0x00, 0x00, 0x00, 0x01])
    out.append(ppsPointer, count: ppsSize)
    return out
}

// MARK: - NetworkTransportDelegate

extension CaptureSession: NetworkTransportDelegate {

    public func transportDidConnect(_ transport: NetworkTransport) {
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.delegate?.captureSessionDidConnectTransport(self)
        }
    }

    public func transportDidDisconnect(_ transport: NetworkTransport, error: Error?) {
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.delegate?.captureSession(self, didDisconnectTransport: error)
        }
    }

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
        encoder.requestKeyframe()
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.delegate?.captureSession(self, didApplyBackpressureBitrate: reducedBitrate)
        }
    }
}

// MARK: - CaptureSessionError

public enum CaptureSessionError: Error, LocalizedError {
    case noCameraAvailable
    case cameraPermissionDenied
    case cannotAddInput
    case cannotAddOutput

    public var errorDescription: String? {
        switch self {
        case .noCameraAvailable: return "No camera device available on this hardware."
        case .cameraPermissionDenied: return "Camera permission is denied. Enable it in Settings > Privacy > Camera."
        case .cannotAddInput:    return "Cannot add camera input to capture session."
        case .cannotAddOutput:   return "Cannot add video output to capture session."
        }
    }
}

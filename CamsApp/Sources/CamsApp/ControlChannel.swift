// ControlChannel.swift
// Cams — UDP back-channel for receiving remote control commands from OBS.
//
// Listens on port 8889 (kCamsControlPort) for CamsControlPacket messages.
// Currently handles:
//   • lockFocus / unlockFocus  — toggle continuous auto-focus.
//   • lockExposure / unlockExposure — toggle continuous auto-exposure.
//   • requestKeyframe — force an IDR frame on the encoder.
//   • ping / pong — latency measurement.

import AVFoundation
import Network
import Foundation

// MARK: - ControlChannelDelegate

public protocol ControlChannelDelegate: AnyObject, Sendable {
    /// The OBS plugin has requested a focus lock/unlock.
    func controlChannel(_ channel: ControlChannel, didReceiveFocusLock locked: Bool)
    /// The OBS plugin has requested an exposure lock/unlock.
    func controlChannel(_ channel: ControlChannel, didReceiveExposureLock locked: Bool)
    /// The OBS plugin has requested an immediate IDR keyframe.
    func controlChannelDidRequestKeyframe(_ channel: ControlChannel)
    /// The OBS plugin has requested a video quality preset.
    func controlChannel(_ channel: ControlChannel, didRequestQuality quality: StreamQuality)
    /// The OBS plugin has requested a Center Stage enable/disable.
    func controlChannel(_ channel: ControlChannel, didSetCenterStageEnabled enabled: Bool)
    /// The OBS plugin has sent a control packet, revealing its host endpoint.
    func controlChannel(_ channel: ControlChannel, didDiscoverOBSEndpoint host: String, port: UInt16)
    /// A control command failed locally before it could be applied.
    func controlChannel(_ channel: ControlChannel, didFailWithError error: Error)
}

// MARK: - ControlChannel

/// Listens for incoming UDP control commands from the OBS plugin.
public final class ControlChannel: @unchecked Sendable {

    // MARK: Public state

    public weak var delegate: (any ControlChannelDelegate)?
    public weak var captureDevice: AVCaptureDevice?
    public weak var encoder: VideoEncoder?

    // MARK: Private state

    private var listener: NWListener?
    private var activeConnections: [NWConnection] = []
    private let queue = DispatchQueue(label: "com.cams.control", qos: .utility)
    private var isStopped = false

    // MARK: - Lifecycle

    public init() {}

    deinit { stop() }

    // MARK: - Public API

    /// Starts listening on `kCamsControlPort` (8889).
    public func start() throws {
        isStopped = false
        let params = NWParameters.udp
        params.allowLocalEndpointReuse = true

        let l = try NWListener(
            using: params,
            on: NWEndpoint.Port(rawValue: kCamsControlPort)!
        )

        l.stateUpdateHandler = { [weak self] state in
            guard let self else { return }
            switch state {
            case .failed(let error):
                self.notifyFailure(error)
                // Listener failed — retry after a short delay.
                self.queue.asyncAfter(deadline: .now() + 2.0) {
                    guard !self.isStopped else { return }
                    do {
                        try self.start()
                    } catch {
                        self.notifyFailure(error)
                    }
                }
            case .cancelled:
                if !self.isStopped {
                    self.queue.asyncAfter(deadline: .now() + 1.0) {
                        do {
                            try self.start()
                        } catch {
                            self.notifyFailure(error)
                        }
                    }
                }
            default:
                break
            }
        }

        l.newConnectionHandler = { [weak self] connection in
            self?.handleNewConnection(connection)
        }

        l.start(queue: queue)
        listener = l
    }

    /// Stops listening.
    public func stop() {
        queue.async { [weak self] in
            guard let self else { return }
            self.isStopped = true
            self.listener?.cancel()
            self.listener = nil
            self.activeConnections.forEach { $0.cancel() }
            self.activeConnections.removeAll()
        }
    }

    // MARK: - Private helpers

    private func handleNewConnection(_ connection: NWConnection) {
        connection.stateUpdateHandler = { [weak self, weak connection] state in
            guard let self, let connection else { return }
            if case .failed = state {
                self.queue.async {
                    self.activeConnections.removeAll { $0 === connection }
                }
            } else if case .cancelled = state {
                self.queue.async {
                    self.activeConnections.removeAll { $0 === connection }
                }
            }
        }

        receiveNextMessage(on: connection)
        connection.start(queue: queue)
        activeConnections.append(connection)
    }

    private func receiveNextMessage(on connection: NWConnection) {
        connection.receiveMessage { [weak self, weak connection] data, _, isComplete, error in
            guard let self, let connection else { return }
            if let data, let packet = CamsControlPacket.deserialise(from: data) {
                self.handleCommand(packet, replyTo: connection)
            } else if let data, !data.isEmpty {
                self.notifyFailure(ControlChannelError.invalidCommand)
            }
            if let error {
                self.notifyFailure(error)
            }
            if error == nil && !self.isStopped {
                self.receiveNextMessage(on: connection)
            }
            _ = isComplete
        }
    }

    private func handleCommand(_ packet: CamsControlPacket, replyTo connection: NWConnection) {
        notifyDiscoveredEndpoint(from: connection)

        switch packet.command {
        case .lockFocus:
            if applyFocusMode(.locked) {
                delegate?.controlChannel(self, didReceiveFocusLock: true)
            }

        case .unlockFocus:
            if applyFocusMode(.continuousAutoFocus) {
                delegate?.controlChannel(self, didReceiveFocusLock: false)
            }

        case .lockExposure:
            if applyExposureMode(.locked) {
                delegate?.controlChannel(self, didReceiveExposureLock: true)
            }

        case .unlockExposure:
            if applyExposureMode(.continuousAutoExposure) {
                delegate?.controlChannel(self, didReceiveExposureLock: false)
            }

        case .requestKeyframe:
            encoder?.requestKeyframe()
            delegate?.controlChannelDidRequestKeyframe(self)

        case .setQualityLow:
            delegate?.controlChannel(self, didRequestQuality: .low)

        case .setQualityMedium:
            delegate?.controlChannel(self, didRequestQuality: .medium)

        case .setQualityHigh:
            delegate?.controlChannel(self, didRequestQuality: .high)

        case .setAudioEnabled:
            // Reserved for a future audio-capable protocol revision.
            break

        case .setCenterStageEnabled:
            let enabled = packet.payload.first == 0x01
            DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                self.delegate?.controlChannel(self, didSetCenterStageEnabled: enabled)
            }

        case .ping:
            // Reply with a pong on the same connection.
            let pong = CamsControlPacket(command: .pong, payload: packet.payload)
            connection.send(content: pong.serialise(), completion: .idempotent)

        case .pong:
            // Pongs are handled by the OBS side; nothing to do here.
            break
        }
    }

    // MARK: - Camera control helpers

    private func applyFocusMode(_ mode: AVCaptureDevice.FocusMode) -> Bool {
        guard let device = captureDevice else {
            notifyFailure(ControlChannelError.noCaptureDevice)
            return false
        }
        do {
            try device.lockForConfiguration()
            defer { device.unlockForConfiguration() }
            if device.isFocusModeSupported(mode) {
                device.focusMode = mode
                return true
            } else {
                notifyFailure(ControlChannelError.unsupportedFocusMode)
                return false
            }
        } catch {
            notifyFailure(error)
            return false
        }
    }

    private func applyExposureMode(_ mode: AVCaptureDevice.ExposureMode) -> Bool {
        guard let device = captureDevice else {
            notifyFailure(ControlChannelError.noCaptureDevice)
            return false
        }
        do {
            try device.lockForConfiguration()
            defer { device.unlockForConfiguration() }
            if device.isExposureModeSupported(mode) {
                device.exposureMode = mode
                return true
            } else {
                notifyFailure(ControlChannelError.unsupportedExposureMode)
                return false
            }
        } catch {
            notifyFailure(error)
            return false
        }
    }

    private func notifyFailure(_ error: Error) {
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.delegate?.controlChannel(self, didFailWithError: error)
        }
    }

    private func notifyDiscoveredEndpoint(from connection: NWConnection) {
        guard case let .hostPort(host, port) = connection.endpoint else { return }
        let hostString = "\(host)"
        let portValue = port.rawValue
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.delegate?.controlChannel(self, didDiscoverOBSEndpoint: hostString, port: portValue)
        }
    }
}

public enum ControlChannelError: LocalizedError {
    case invalidCommand
    case noCaptureDevice
    case unsupportedFocusMode
    case unsupportedExposureMode

    public var errorDescription: String? {
        switch self {
        case .invalidCommand:
            return "Received an invalid control command."
        case .noCaptureDevice:
            return "No active camera is available for remote control."
        case .unsupportedFocusMode:
            return "The selected camera does not support the requested focus mode."
        case .unsupportedExposureMode:
            return "The selected camera does not support the requested exposure mode."
        }
    }
}

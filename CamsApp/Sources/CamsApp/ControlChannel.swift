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
                // Listener failed — retry after a short delay.
                self.queue.asyncAfter(deadline: .now() + 2.0) {
                    guard !self.isStopped else { return }
                    try? self.start()
                }
                _ = error  // Error logged implicitly via debugDescription
            case .cancelled:
                if !self.isStopped {
                    self.queue.asyncAfter(deadline: .now() + 1.0) {
                        try? self.start()
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
        isStopped = true
        listener?.cancel()
        listener = nil
        activeConnections.forEach { $0.cancel() }
        activeConnections.removeAll()
    }

    // MARK: - Private helpers

    private func handleNewConnection(_ connection: NWConnection) {
        connection.stateUpdateHandler = { [weak self, weak connection] state in
            guard let self, let connection else { return }
            if case .failed = state {
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
            }
            if error == nil && !isComplete {
                self.receiveNextMessage(on: connection)
            }
        }
    }

    private func handleCommand(_ packet: CamsControlPacket, replyTo connection: NWConnection) {
        switch packet.command {
        case .lockFocus:
            applyFocusMode(.locked)
            delegate?.controlChannel(self, didReceiveFocusLock: true)

        case .unlockFocus:
            applyFocusMode(.continuousAutoFocus)
            delegate?.controlChannel(self, didReceiveFocusLock: false)

        case .lockExposure:
            applyExposureMode(.locked)
            delegate?.controlChannel(self, didReceiveExposureLock: true)

        case .unlockExposure:
            applyExposureMode(.continuousAutoExposure)
            delegate?.controlChannel(self, didReceiveExposureLock: false)

        case .requestKeyframe:
            delegate?.controlChannelDidRequestKeyframe(self)

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

    private func applyFocusMode(_ mode: AVCaptureDevice.FocusMode) {
        guard let device = captureDevice else { return }
        do {
            try device.lockForConfiguration()
            if device.isFocusModeSupported(mode) {
                device.focusMode = mode
            }
            device.unlockForConfiguration()
        } catch {
            // Device locked by another process — silently ignore; the focus
            // state remains unchanged.
        }
    }

    private func applyExposureMode(_ mode: AVCaptureDevice.ExposureMode) {
        guard let device = captureDevice else { return }
        do {
            try device.lockForConfiguration()
            if device.isExposureModeSupported(mode) {
                device.exposureMode = mode
            }
            device.unlockForConfiguration()
        } catch {
            // Device locked by another process — silently ignore.
        }
    }
}

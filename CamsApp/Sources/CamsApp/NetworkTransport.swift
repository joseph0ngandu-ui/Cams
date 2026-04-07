// NetworkTransport.swift
// Cams — UDP transport layer using Apple's Network.framework.
//
// Features:
//  • Custom-header packetisation (CamsPacketHeader, 17 bytes).
//  • Backpressure: monitors NWConnection.State and the send-buffer depth.
//    If the outbound queue reaches `maxQueueDepth`, frames are dropped
//    (or the encoder's QP is raised via the delegate) rather than
//    allowing lag to accumulate.
//  • Automatic reconnection with exponential back-off (up to 30 s).

import Network
import Foundation

// MARK: - NetworkTransportDelegate

/// Receives transport-layer events.
public protocol NetworkTransportDelegate: AnyObject, Sendable {
    /// Called when the connection transitions to `.ready`.
    func transportDidConnect(_ transport: NetworkTransport)
    /// Called when the connection is lost or closed.
    func transportDidDisconnect(_ transport: NetworkTransport, error: Error?)
    /// Called when backpressure triggers and the caller should reduce bitrate or
    /// drop the current frame.  `queueDepth` is the current pending-send count.
    func transportNeedsBackpressure(_ transport: NetworkTransport, queueDepth: Int)
}

// MARK: - NetworkTransport

/// Sends packetised video data over a UDP connection to an OBS plugin endpoint.
public final class NetworkTransport: @unchecked Sendable {

    // MARK: Configuration

    /// Maximum number of packets queued for sending before backpressure fires.
    public var maxQueueDepth: Int = 8

    // MARK: Public state

    public weak var delegate: (any NetworkTransportDelegate)?
    /// Whether the connection is ready to send.
    public private(set) var isReady: Bool = false

    // MARK: Private state

    private var connection: NWConnection?
    private let queue = DispatchQueue(label: "com.cams.networkTransport", qos: .userInteractive)
    private var sequenceNumber: UInt32 = 0
    private var pendingSendCount: Int = 0
    private var reconnectDelay: TimeInterval = 1.0
    private var reconnectTimer: DispatchSourceTimer?
    private var targetHost: String = ""
    private var targetPort: UInt16 = kCamsVideoPort
    private var isStopped = false

    // MARK: - Lifecycle

    public init() {}

    deinit {
        stopInternal()
    }

    // MARK: - Public API

    /// Connects to `host`:`port` over UDP.
    public func connect(host: String, port: UInt16 = kCamsVideoPort) {
        queue.async { [weak self] in
            guard let self else { return }
            self.targetHost = host
            self.targetPort = port
            self.isStopped  = false
            self.reconnectDelay = 1.0
            self.openConnection()
        }
    }

    /// Disconnects and cancels any pending reconnection.
    public func disconnect() {
        queue.async { [weak self] in
            self?.isStopped = true
            self?.stopInternal()
        }
    }

    /// Packetises and sends an encoded video payload.
    ///
    /// If the send queue is at capacity the frame is silently dropped and the
    /// delegate is informed so it can reduce quality.
    ///
    /// - Parameters:
    ///   - data: Raw encoded payload bytes (NAL units).
    ///   - frameType: Semantic type of the enclosed data.
    public func send(data: Data, frameType: FrameType) {
        queue.async { [weak self] in
            guard let self, self.isReady else { return }

            // Backpressure check — drop if buffer is full.
            if self.pendingSendCount >= self.maxQueueDepth {
                self.delegate?.transportNeedsBackpressure(self, queueDepth: self.pendingSendCount)
                return
            }

            let seqNum = self.sequenceNumber
            self.sequenceNumber &+= 1

            let header = CamsPacketHeader(
                sequenceNumber: seqNum,
                timestamp:      camsCurrentTimestampNs(),
                frameType:      frameType,
                payloadLength:  UInt32(data.count)
            )

            var packet = header.serialise()
            packet.append(data)

            self.pendingSendCount += 1
            self.connection?.send(
                content: packet,
                completion: .contentProcessed { [weak self] error in
                    guard let self else { return }
                    self.queue.async {
                        self.pendingSendCount = max(0, self.pendingSendCount - 1)
                        if let error {
                            // Non-fatal send error; log and continue.
                            // Fatal connection errors are handled in stateUpdateHandler.
                            let nsError = error as NSError
                            guard nsError.domain == NWError.posix(.ECANCELED).localizedDescription
                            else { return }
                        }
                    }
                }
            )
        }
    }

    // MARK: - Private helpers

    private func openConnection() {
        let endpoint = NWEndpoint.hostPort(
            host: NWEndpoint.Host(targetHost),
            port: NWEndpoint.Port(rawValue: targetPort)!
        )
        let params = NWParameters.udp
        params.allowLocalEndpointReuse = true
        params.allowFastOpen           = true

        let conn = NWConnection(to: endpoint, using: params)
        connection = conn

        conn.stateUpdateHandler = { [weak self] state in
            self?.queue.async {
                self?.handleStateChange(state)
            }
        }

        conn.start(queue: queue)
    }

    private func handleStateChange(_ state: NWConnection.State) {
        switch state {
        case .ready:
            isReady = true
            reconnectDelay = 1.0
            cancelReconnectTimer()
            DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                self.delegate?.transportDidConnect(self)
            }

        case .failed(let error):
            isReady = false
            DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                self.delegate?.transportDidDisconnect(self, error: error)
            }
            scheduleReconnect()

        case .cancelled:
            isReady = false
            if !isStopped {
                scheduleReconnect()
            }

        case .waiting(let error):
            // Network temporarily unavailable — NWConnection will retry internally,
            // but we also schedule our own reconnect to reset the session state.
            isReady = false
            DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                self.delegate?.transportDidDisconnect(self, error: error)
            }
            scheduleReconnect()

        default:
            break
        }
    }

    private func scheduleReconnect() {
        guard !isStopped else { return }
        cancelReconnectTimer()

        let delay = reconnectDelay
        // Exponential back-off capped at 30 seconds.
        reconnectDelay = min(reconnectDelay * 2.0, 30.0)

        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + delay)
        timer.setEventHandler { [weak self] in
            guard let self, !self.isStopped else { return }
            self.stopInternal()
            self.openConnection()
        }
        timer.resume()
        reconnectTimer = timer
    }

    private func cancelReconnectTimer() {
        reconnectTimer?.cancel()
        reconnectTimer = nil
    }

    /// Must be called on `queue`.
    private func stopInternal() {
        cancelReconnectTimer()
        isReady = false
        pendingSendCount = 0
        connection?.cancel()
        connection = nil
    }
}

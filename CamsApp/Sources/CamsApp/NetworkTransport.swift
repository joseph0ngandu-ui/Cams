// NetworkTransport.swift
// Cams — UDP transport layer using Apple's Network.framework.
//
// Features:
//  • Custom-header packetisation (CamsPacketHeader, 21 bytes).
//  • Backpressure: monitors NWConnection.State and the send-buffer depth.
//    If the outbound queue reaches `maxQueueDepth`, frames are dropped
//    (or the encoder's QP is raised via the delegate) rather than
//    allowing lag to accumulate.
//  • Automatic reconnection with exponential back-off (up to 30 s).

import Network
import Foundation

// MARK: - Packetisation

internal enum CamsPacketisationError: Error, Equatable {
    case invalidFragmentPayloadSize
    case tooManyFragments(fragmentCount: Int, maxFragments: Int)
}

internal struct CamsFramePacketizer {
    static let defaultFragmentPayloadSize = 1200

    private(set) var nextSequenceNumber: UInt32 = 0

    mutating func packetise(
        data: Data,
        frameType: FrameType,
        timestamp: UInt64,
        fragmentPayloadSize: Int = Self.defaultFragmentPayloadSize
    ) throws -> [Data] {
        guard fragmentPayloadSize > 0 else {
            throw CamsPacketisationError.invalidFragmentPayloadSize
        }

        let totalFragments = max(1, (data.count + fragmentPayloadSize - 1) / fragmentPayloadSize)
        guard totalFragments <= Int(kCamsMaxFragmentsPerFrame) else {
            throw CamsPacketisationError.tooManyFragments(
                fragmentCount: totalFragments,
                maxFragments: Int(kCamsMaxFragmentsPerFrame)
            )
        }

        let frameSequence = nextSequenceNumber
        nextSequenceNumber &+= 1

        var packets: [Data] = []
        packets.reserveCapacity(totalFragments)

        for fragmentIndex in 0..<totalFragments {
            let offset = fragmentIndex * fragmentPayloadSize
            let length = min(fragmentPayloadSize, data.count - offset)
            let payloadRange = offset..<(offset + length)
            let payload = data.subdata(in: payloadRange)

            let header = CamsPacketHeader(
                sequenceNumber: frameSequence,
                timestamp: timestamp,
                frameType: frameType,
                fragmentIndex: UInt16(fragmentIndex),
                fragmentCount: UInt16(totalFragments),
                payloadLength: UInt32(payload.count)
            )

            var packet = header.serialise()
            packet.append(payload)
            packets.append(packet)
        }

        return packets
    }
}

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
    public var maxQueueDepth: Int = 800

    // MARK: Public state

    public weak var delegate: (any NetworkTransportDelegate)?
    /// Whether the connection is ready to send.
    public private(set) var isReady: Bool = false

    // MARK: Private state

    private var connection: NWConnection?
    private let queue = DispatchQueue(label: "com.cams.networkTransport", qos: .userInteractive)
    private var packetizer = CamsFramePacketizer()
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
            let trimmedHost = host.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !trimmedHost.isEmpty else { return }
            self.targetHost = trimmedHost
            self.targetPort = port
            self.isStopped  = false
            self.reconnectDelay = 1.0
            self.stopInternal()
            self.isStopped = false
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

            let timestamp = camsCurrentTimestampNs()
            let fragmentPayloadSize = CamsFramePacketizer.defaultFragmentPayloadSize
            let totalFragments = max(1, (data.count + fragmentPayloadSize - 1) / fragmentPayloadSize)

            guard totalFragments <= Int(kCamsMaxFragmentsPerFrame),
                  self.pendingSendCount + totalFragments <= self.maxQueueDepth
            else {
                self.delegate?.transportNeedsBackpressure(self, queueDepth: self.pendingSendCount)
                return
            }

            let packets: [Data]
            do {
                packets = try self.packetizer.packetise(
                    data: data,
                    frameType: frameType,
                    timestamp: timestamp,
                    fragmentPayloadSize: fragmentPayloadSize
                )
            } catch {
                self.delegate?.transportNeedsBackpressure(self, queueDepth: self.pendingSendCount)
                return
            }

            for packet in packets {
                self.pendingSendCount += 1
                self.connection?.send(
                    content: packet,
                    completion: .contentProcessed { [weak self] error in
                        guard let self else { return }
                        self.queue.async {
                            self.pendingSendCount = max(0, self.pendingSendCount - 1)
                            if let error,
                               case .posix(let posixCode) = error,
                               posixCode == POSIXErrorCode.ECANCELED {
                                // Socket was cancelled deliberately — not an error condition.
                                return
                            }
                        }
                    }
                )
            }
        }
    }

    // MARK: - Private helpers

    private func openConnection() {
        guard !targetHost.isEmpty, let port = NWEndpoint.Port(rawValue: targetPort) else {
            return
        }
        let endpoint = NWEndpoint.hostPort(
            host: NWEndpoint.Host(targetHost),
            port: port
        )
        let params = NWParameters.udp
        params.allowLocalEndpointReuse = true
        params.allowFastOpen           = true

        let conn = NWConnection(to: endpoint, using: params)
        connection = conn

        conn.stateUpdateHandler = { [weak self] state in
            guard let transport = self else { return }
            transport.queue.async {
                transport.handleStateChange(state)
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

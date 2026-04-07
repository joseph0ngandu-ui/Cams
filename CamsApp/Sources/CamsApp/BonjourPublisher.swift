// BonjourPublisher.swift
// Cams — Zero-config device discovery via Bonjour (NSNetService).
//
// Advertises the device under the "_cams-video._udp" service type so that
// the OBS plugin can locate it without any manual IP configuration.
// Automatically re-publishes on failures such as name conflicts.

import Foundation
import Network

// MARK: - BonjourPublisherDelegate

public protocol BonjourPublisherDelegate: AnyObject, Sendable {
    /// Called when the service has been successfully published.
    func bonjourPublisherDidPublish(_ publisher: BonjourPublisher, name: String)
    /// Called when publishing fails.  The publisher will automatically retry.
    func bonjourPublisherDidFail(_ publisher: BonjourPublisher, error: Error)
}

// MARK: - BonjourPublisher

/// Advertises the Cams video streaming service over Bonjour.
///
/// Uses `NWListener` (Network.framework) to both listen for incoming
/// connections and advertise via Bonjour simultaneously, which is the
/// modern replacement for `NSNetService` on iOS 14+.
public final class BonjourPublisher: @unchecked Sendable {

    // MARK: Public state

    public weak var delegate: (any BonjourPublisherDelegate)?
    /// The port the listener is bound to (assigned after `start()`).
    public private(set) var port: UInt16 = 0

    // MARK: Private state

    private var listener: NWListener?
    private let queue = DispatchQueue(label: "com.cams.bonjour", qos: .utility)
    private var serviceName: String
    private var retryCount: Int = 0
    private var isStopped  = false

    // MARK: - Lifecycle

    /// - Parameter serviceName: The human-readable name shown in the OBS plugin
    ///   device picker.  Defaults to the device name.
    public init(serviceName: String? = nil) {
        self.serviceName = serviceName ?? UIDevice.current.name
    }

    deinit { stop() }

    // MARK: - Public API

    /// Starts advertising the service.  Safe to call multiple times — an
    /// existing listener is cancelled before a new one is started.
    public func start() {
        queue.async { [weak self] in
            guard let self else { return }
            self.isStopped = false
            self.startListener()
        }
    }

    /// Stops advertising and releases the listener.
    public func stop() {
        queue.async { [weak self] in
            guard let self else { return }
            self.isStopped = true
            self.listener?.cancel()
            self.listener = nil
        }
    }

    // MARK: - Private

    private func startListener() {
        listener?.cancel()

        let params = NWParameters.udp
        params.allowLocalEndpointReuse = true

        do {
            let l = try NWListener(using: params)

            // Advertise with Bonjour.
            l.service = NWListener.Service(
                name: serviceName,
                type: kCamsServiceType
            )

            l.serviceRegistrationUpdateHandler = { [weak self] change in
                guard let self else { return }
                switch change {
                case .add(let endpoint):
                    if case let .service(name, _, _, _) = endpoint {
                        self.port = l.port?.rawValue ?? 0
                        let publishedName = name
                        DispatchQueue.main.async {
                            self.delegate?.bonjourPublisherDidPublish(self, name: publishedName)
                        }
                    }
                case .remove:
                    break
                @unknown default:
                    break
                }
            }

            l.stateUpdateHandler = { [weak self] state in
                guard let self else { return }
                switch state {
                case .ready:
                    self.retryCount = 0
                    self.port = l.port?.rawValue ?? 0

                case .failed(let error):
                    DispatchQueue.main.async {
                        self.delegate?.bonjourPublisherDidFail(self, error: error)
                    }
                    // Retry with back-off.
                    self.scheduleRetry()

                case .cancelled:
                    if !self.isStopped { self.scheduleRetry() }

                default:
                    break
                }
            }

            // We don't need to handle new connections (UDP is connectionless),
            // but NWListener requires a newConnectionHandler to stay active.
            l.newConnectionHandler = { connection in
                connection.cancel()
            }

            l.start(queue: queue)
            listener = l

        } catch {
            DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                self.delegate?.bonjourPublisherDidFail(self, error: error)
            }
            scheduleRetry()
        }
    }

    private func scheduleRetry() {
        guard !isStopped else { return }
        retryCount += 1
        let delay = min(Double(retryCount) * 2.0, 30.0)
        queue.asyncAfter(deadline: .now() + delay) { [weak self] in
            guard let self, !self.isStopped else { return }
            self.startListener()
        }
    }
}

// MARK: - UIDevice shim for non-UIKit targets (tests / macOS)
// When running on platforms without UIKit (e.g., unit tests on macOS),
// provide a fallback device name.
#if canImport(UIKit)
import UIKit
#else
private enum UIDevice {
    static let current = _FakeDevice()
    struct _FakeDevice { let name = "Cams-Device" }
}
#endif

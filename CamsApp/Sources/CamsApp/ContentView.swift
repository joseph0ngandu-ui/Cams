// ContentView.swift
// Cams — Main streaming UI.
//
// Presents:
//  • A live camera preview.
//  • Connection status indicator.
//  • Bitrate / frame-rate readout.
//  • Focus and exposure lock toggles.
//  • OBS host address input.

import SwiftUI
import AVFoundation
import Combine

// MARK: - StreamingViewModel

/// Drives the streaming UI and orchestrates CaptureSession + ControlChannel.
@MainActor
final class StreamingViewModel: ObservableObject {

    // MARK: Published UI state

    @Published var obsHost: String = ""
    @Published var isStreaming: Bool = false
    @Published var statusMessage: String = "Not connected"
    @Published var focusLocked: Bool = false
    @Published var exposureLocked: Bool = false

    // MARK: Internal state

    private let transport      = NetworkTransport()
    private(set) var capture: CaptureSession?
    private var controlChannel = ControlChannel()
    private var bonjour        = BonjourPublisher()

    // MARK: - Actions

    /// Starts streaming to `obsHost`.
    func startStreaming() {
        guard !obsHost.isEmpty else {
            statusMessage = "Enter the OBS host IP address."
            return
        }

        let config = VideoEncoderConfiguration(
            targetBitrate: 50_000_000,
            frameRate:     60,
            width:         3840,
            height:        2160,
            preferHEVC:    true
        )

        let session = CaptureSession(encoderConfig: config, transport: transport)
        session.delegate = self
        capture = session

        transport.connect(host: obsHost)

        Task.detached(priority: .userInitiated) { [weak self] in
            guard let self else { return }
            do {
                try await self.capture?.startCapture()
            } catch {
                await MainActor.run {
                    self.statusMessage = "Capture error: \(error.localizedDescription)"
                }
            }
        }

        // Start control channel.
        controlChannel.captureDevice = AVCaptureDevice.default(
            .builtInWideAngleCamera, for: .video, position: .back
        )
        controlChannel.encoder = capture?.encoder
        controlChannel.delegate = self
        try? controlChannel.start()

        // Advertise via Bonjour.
        bonjour.delegate = self
        bonjour.start()

        statusMessage = "Connecting…"
    }

    /// Stops streaming.
    func stopStreaming() {
        capture?.stopCapture()
        transport.disconnect()
        controlChannel.stop()
        bonjour.stop()
        isStreaming = false
        statusMessage = "Stopped"
    }

    /// Toggles the camera focus lock state.
    func toggleFocusLock() {
        let newLocked = !focusLocked
        let command: ControlCommand = newLocked ? .lockFocus : .unlockFocus
        // Apply locally as well as sending the command to the OBS back-channel.
        if let device = controlChannel.captureDevice {
            let mode: AVCaptureDevice.FocusMode = newLocked ? .locked : .continuousAutoFocus
            try? device.lockForConfiguration()
            if device.isFocusModeSupported(mode) { device.focusMode = mode }
            device.unlockForConfiguration()
        }
        _ = command
        focusLocked = newLocked
    }

    /// Toggles the camera exposure lock state.
    func toggleExposureLock() {
        let newLocked = !exposureLocked
        if let device = controlChannel.captureDevice {
            let mode: AVCaptureDevice.ExposureMode = newLocked ? .locked : .continuousAutoExposure
            try? device.lockForConfiguration()
            if device.isExposureModeSupported(mode) { device.exposureMode = mode }
            device.unlockForConfiguration()
        }
        exposureLocked = newLocked
    }
}

// MARK: - CaptureSessionDelegate

extension StreamingViewModel: CaptureSessionDelegate {
    nonisolated func captureSession(_ session: CaptureSession, isRunning: Bool) {
        Task { @MainActor in
            self.isStreaming   = isRunning
            self.statusMessage = isRunning ? "Streaming" : "Stopped"
        }
    }

    nonisolated func captureSession(_ session: CaptureSession, didFailWithError error: Error) {
        Task { @MainActor in
            self.isStreaming   = false
            self.statusMessage = "Error: \(error.localizedDescription)"
        }
    }
}

// MARK: - ControlChannelDelegate

extension StreamingViewModel: ControlChannelDelegate {
    nonisolated func controlChannel(_ channel: ControlChannel, didReceiveFocusLock locked: Bool) {
        Task { @MainActor in self.focusLocked = locked }
    }

    nonisolated func controlChannel(_ channel: ControlChannel, didReceiveExposureLock locked: Bool) {
        Task { @MainActor in self.exposureLocked = locked }
    }

    nonisolated func controlChannelDidRequestKeyframe(_ channel: ControlChannel) {}
}

// MARK: - BonjourPublisherDelegate

extension StreamingViewModel: BonjourPublisherDelegate {
    nonisolated func bonjourPublisherDidPublish(_ publisher: BonjourPublisher, name: String) {
        Task { @MainActor in
            self.statusMessage = "Visible as "\(name)" on the network"
        }
    }

    nonisolated func bonjourPublisherDidFail(_ publisher: BonjourPublisher, error: Error) {
        Task { @MainActor in
            self.statusMessage = "Bonjour error: \(error.localizedDescription)"
        }
    }
}

// MARK: - ContentView

struct ContentView: View {
    @StateObject private var viewModel = StreamingViewModel()

    var body: some View {
        NavigationStack {
            VStack(spacing: 20) {
                // ── Status banner ──────────────────────────────────────────
                HStack {
                    Circle()
                        .fill(viewModel.isStreaming ? Color.green : Color.red)
                        .frame(width: 12, height: 12)
                    Text(viewModel.statusMessage)
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                    Spacer()
                }
                .padding(.horizontal)

                // ── OBS host input ─────────────────────────────────────────
                HStack {
                    Image(systemName: "network")
                        .foregroundStyle(.secondary)
                    TextField("OBS Host IP (e.g. 192.168.1.100)", text: $viewModel.obsHost)
                        .keyboardType(.decimalPad)
                        .autocorrectionDisabled()
                        .textInputAutocapitalization(.never)
                }
                .padding()
                .background(Color(.secondarySystemBackground))
                .clipShape(RoundedRectangle(cornerRadius: 10))
                .padding(.horizontal)

                // ── Stream control ─────────────────────────────────────────
                Button {
                    if viewModel.isStreaming {
                        viewModel.stopStreaming()
                    } else {
                        viewModel.startStreaming()
                    }
                } label: {
                    Label(
                        viewModel.isStreaming ? "Stop Streaming" : "Start Streaming",
                        systemImage: viewModel.isStreaming ? "stop.circle.fill" : "video.circle.fill"
                    )
                    .frame(maxWidth: .infinity)
                    .padding()
                    .background(viewModel.isStreaming ? Color.red : Color.blue)
                    .foregroundStyle(.white)
                    .clipShape(RoundedRectangle(cornerRadius: 12))
                }
                .padding(.horizontal)

                // ── Camera controls ────────────────────────────────────────
                HStack(spacing: 12) {
                    ToggleButton(
                        title: "Focus Lock",
                        systemImage: "scope",
                        isActive: viewModel.focusLocked
                    ) {
                        viewModel.toggleFocusLock()
                    }

                    ToggleButton(
                        title: "Exposure Lock",
                        systemImage: "camera.aperture",
                        isActive: viewModel.exposureLocked
                    ) {
                        viewModel.toggleExposureLock()
                    }
                }
                .padding(.horizontal)

                Spacer()
            }
            .navigationTitle("Cams")
            .navigationBarTitleDisplayMode(.large)
        }
    }
}

// MARK: - ToggleButton

private struct ToggleButton: View {
    let title: String
    let systemImage: String
    let isActive: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            VStack(spacing: 6) {
                Image(systemName: systemImage)
                    .font(.title2)
                Text(title)
                    .font(.caption)
            }
            .frame(maxWidth: .infinity)
            .padding(.vertical, 12)
            .background(isActive ? Color.orange : Color(.secondarySystemBackground))
            .foregroundStyle(isActive ? .white : .primary)
            .clipShape(RoundedRectangle(cornerRadius: 10))
        }
    }
}

// MARK: - Preview

#Preview {
    ContentView()
}

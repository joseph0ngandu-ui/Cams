// ContentView.swift
// Cams - Main streaming UI.

import SwiftUI
import AVFoundation
import Combine
import Darwin

public enum StreamQuality: String, CaseIterable, Identifiable, Sendable {
    case low = "Low (720p30)"
    case medium = "Medium (1080p30)"
    case high = "High (4K60)"

    public var id: String { rawValue }

    var title: String {
        switch self {
        case .low: return "720p"
        case .medium: return "1080p"
        case .high: return "4K"
        }
    }

    var detail: String {
        switch self {
        case .low: return "30"
        case .medium: return "30"
        case .high: return "60"
        }
    }

    var encoderConfig: VideoEncoderConfiguration {
        switch self {
        case .low:
            return .init(targetBitrate: 4_000_000, frameRate: 30, width: 1280, height: 720, preferHEVC: true)
        case .medium:
            return .init(targetBitrate: 8_000_000, frameRate: 30, width: 1920, height: 1080, preferHEVC: true)
        case .high:
            return .init(targetBitrate: 50_000_000, frameRate: 60, width: 3840, height: 2160, preferHEVC: true)
        }
    }
}

// MARK: - StreamingViewModel

@MainActor
final class StreamingViewModel: ObservableObject {
    @Published var isStreaming = false
    @Published var isStarting = false
    @Published var isOBSConnected = false
    @Published var statusMessage = "Ready"
    @Published var focusLocked = false
    @Published var exposureLocked = false
    @Published var selectedQuality: StreamQuality = .high
    @Published var localIPAddress = "-"
    @Published var localVideoPort: UInt16 = kCamsVideoPort
    @Published var localControlPort: UInt16 = kCamsControlPort
    @Published var previewSession: AVCaptureSession?
    @Published var activeBitrateMbps: Double?

    private let transport = NetworkTransport()
    private(set) var capture: CaptureSession?
    private var controlChannel = ControlChannel()
    private var bonjour = BonjourPublisher()
    private var startTask: Task<Void, Never>?

    var connectionTitle: String {
        if isOBSConnected { return "OBS linked" }
        if isStarting { return "Starting camera" }
        if isStreaming { return "Camera live" }
        return "Idle"
    }

    var statusTint: Color {
        if isOBSConnected { return .green }
        if isStarting || isStreaming { return .orange }
        return .secondary
    }

    func startStreaming() {
        guard !isStarting, !isStreaming else { return }

        localIPAddress = Self.detectLocalIPv4() ?? "-"
        activeBitrateMbps = Double(selectedQuality.encoderConfig.targetBitrate) / 1_000_000
        isStarting = true
        statusMessage = "Starting camera..."

        let session = CaptureSession(encoderConfig: selectedQuality.encoderConfig, transport: transport)
        session.delegate = self
        capture = session
        previewSession = session.previewSession

        controlChannel.captureDevice = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back)
        controlChannel.encoder = session.encoder
        controlChannel.delegate = self

        do {
            try controlChannel.start()
            bonjour.delegate = self
            bonjour.start()
        } catch {
            handleStartupFailure(error, session: session)
            return
        }

        startTask?.cancel()
        startTask = Task { [weak self, session] in
            do {
                try await Task.detached(priority: .userInitiated) {
                    try session.startCapture()
                }.value

                await MainActor.run {
                    guard let self, self.capture === session else { return }
                    self.controlChannel.captureDevice = session.captureDevice
                    self.isStarting = false
                    self.isStreaming = true
                    self.statusMessage = self.isOBSConnected ? "Streaming to OBS" : "Waiting for OBS control link"
                }
            } catch {
                await MainActor.run {
                    self?.handleStartupFailure(error, session: session)
                }
            }
        }
    }

    func stopStreaming() {
        startTask?.cancel()
        startTask = nil
        capture?.stopCapture()
        transport.disconnect()
        controlChannel.stop()
        bonjour.stop()
        capture = nil
        previewSession = nil
        isStarting = false
        isStreaming = false
        isOBSConnected = false
        activeBitrateMbps = nil
        statusMessage = "Stopped"
    }

    func applyQuality(_ quality: StreamQuality) {
        selectedQuality = quality
        activeBitrateMbps = Double(quality.encoderConfig.targetBitrate) / 1_000_000
        capture?.updateEncoderConfiguration(quality.encoderConfig)
        capture?.requestKeyframe()
        statusMessage = "Quality set to \(quality.rawValue)"
    }

    func toggleFocusLock() {
        setFocusLocked(!focusLocked)
    }

    func toggleExposureLock() {
        setExposureLocked(!exposureLocked)
    }

    private func setFocusLocked(_ locked: Bool) {
        let mode: AVCaptureDevice.FocusMode = locked ? .locked : .continuousAutoFocus
        guard let device = controlChannel.captureDevice else {
            statusMessage = "No active camera to focus"
            return
        }

        do {
            try device.lockForConfiguration()
            defer { device.unlockForConfiguration() }
            guard device.isFocusModeSupported(mode) else {
                statusMessage = "Focus mode unavailable"
                return
            }
            device.focusMode = mode
            focusLocked = locked
            statusMessage = locked ? "Focus locked" : "Focus continuous"
        } catch {
            statusMessage = "Focus failed: \(error.localizedDescription)"
        }
    }

    private func setExposureLocked(_ locked: Bool) {
        let mode: AVCaptureDevice.ExposureMode = locked ? .locked : .continuousAutoExposure
        guard let device = controlChannel.captureDevice else {
            statusMessage = "No active camera to expose"
            return
        }

        do {
            try device.lockForConfiguration()
            defer { device.unlockForConfiguration() }
            guard device.isExposureModeSupported(mode) else {
                statusMessage = "Exposure mode unavailable"
                return
            }
            device.exposureMode = mode
            exposureLocked = locked
            statusMessage = locked ? "Exposure locked" : "Exposure continuous"
        } catch {
            statusMessage = "Exposure failed: \(error.localizedDescription)"
        }
    }

    private func handleStartupFailure(_ error: Error, session: CaptureSession) {
        if capture === session {
            capture = nil
            previewSession = nil
        }
        session.stopCapture()
        transport.disconnect()
        controlChannel.stop()
        bonjour.stop()
        isStarting = false
        isStreaming = false
        isOBSConnected = false
        statusMessage = "Camera error: \(error.localizedDescription)"
    }
}

// MARK: - CaptureSessionDelegate

extension StreamingViewModel: CaptureSessionDelegate {
    nonisolated func captureSession(_ session: CaptureSession, isRunning: Bool) {
        Task { @MainActor in
            guard self.capture === session else { return }
            self.isStreaming = isRunning
            self.isStarting = false
            self.statusMessage = isRunning ? "Waiting for OBS control link" : "Stopped"
        }
    }

    nonisolated func captureSession(_ session: CaptureSession, didFailWithError error: Error) {
        Task { @MainActor in
            guard self.capture === session else { return }
            self.isStreaming = false
            self.isStarting = false
            self.statusMessage = "Error: \(error.localizedDescription)"
        }
    }

    nonisolated func captureSessionDidConnectTransport(_ session: CaptureSession) {
        Task { @MainActor in
            guard self.capture === session else { return }
            self.isOBSConnected = true
            self.statusMessage = "Streaming to OBS"
        }
    }

    nonisolated func captureSession(_ session: CaptureSession, didDisconnectTransport error: Error?) {
        Task { @MainActor in
            guard self.capture === session else { return }
            self.isOBSConnected = false
            self.statusMessage = error.map { "OBS link lost: \($0.localizedDescription)" } ?? "OBS link lost"
        }
    }

    nonisolated func captureSession(_ session: CaptureSession, didApplyBackpressureBitrate bitrate: Int) {
        Task { @MainActor in
            guard self.capture === session else { return }
            self.activeBitrateMbps = Double(bitrate) / 1_000_000
            self.statusMessage = "Network pressure; bitrate reduced"
        }
    }
}

// MARK: - ControlChannelDelegate

extension StreamingViewModel: ControlChannelDelegate {
    nonisolated func controlChannel(_ channel: ControlChannel, didReceiveFocusLock locked: Bool) {
        Task { @MainActor in
            self.focusLocked = locked
            self.statusMessage = locked ? "Focus locked by OBS" : "Focus released by OBS"
        }
    }

    nonisolated func controlChannel(_ channel: ControlChannel, didReceiveExposureLock locked: Bool) {
        Task { @MainActor in
            self.exposureLocked = locked
            self.statusMessage = locked ? "Exposure locked by OBS" : "Exposure released by OBS"
        }
    }

    nonisolated func controlChannelDidRequestKeyframe(_ channel: ControlChannel) {
        Task { @MainActor in
            self.capture?.requestKeyframe()
            self.statusMessage = "Keyframe requested by OBS"
        }
    }

    nonisolated func controlChannel(_ channel: ControlChannel, didDiscoverOBSEndpoint host: String, port: UInt16) {
        Task { @MainActor in
            if self.capture == nil && !self.isStarting {
                self.startStreaming()
            }
            self.transport.connect(host: host, port: kCamsVideoPort)
            self.isOBSConnected = true
            self.statusMessage = "OBS linked at \(host)"
        }
    }

    nonisolated func controlChannel(_ channel: ControlChannel, didRequestQuality quality: StreamQuality) {
        Task { @MainActor in
            self.applyQuality(quality)
            self.statusMessage = "Quality set by OBS: \(quality.rawValue)"
        }
    }

    nonisolated func controlChannel(_ channel: ControlChannel, didFailWithError error: Error) {
        Task { @MainActor in
            self.statusMessage = "Control error: \(error.localizedDescription)"
        }
    }
}

// MARK: - BonjourPublisherDelegate

extension StreamingViewModel: BonjourPublisherDelegate {
    nonisolated func bonjourPublisherDidPublish(_ publisher: BonjourPublisher, name: String) {
        Task { @MainActor in
            self.statusMessage = "Visible as \(name)"
            self.localIPAddress = Self.detectLocalIPv4() ?? "-"
        }
    }

    nonisolated func bonjourPublisherDidFail(_ publisher: BonjourPublisher, error: Error) {
        Task { @MainActor in
            self.statusMessage = "Bonjour error: \(error.localizedDescription)"
        }
    }

    private static func detectLocalIPv4() -> String? {
        var address: String?
        var ifaddr: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&ifaddr) == 0, let firstAddr = ifaddr else { return nil }
        defer { freeifaddrs(ifaddr) }

        for ptr in sequence(first: firstAddr, next: { $0.pointee.ifa_next }) {
            let interface = ptr.pointee
            guard let addr = interface.ifa_addr else { continue }
            let family = addr.pointee.sa_family
            guard family == UInt8(AF_INET) else { continue }
            let name = String(cString: interface.ifa_name)
            guard name == "en0" || name == "pdp_ip0" else { continue }

            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            getnameinfo(addr, socklen_t(addr.pointee.sa_len), &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST)
            address = String(cString: host)
            break
        }
        return address
    }
}

// MARK: - ContentView

struct ContentView: View {
    @StateObject private var viewModel = StreamingViewModel()

    var body: some View {
        ZStack {
            CameraPreviewView(session: viewModel.previewSession)
                .ignoresSafeArea()
                .overlay {
                    if viewModel.previewSession == nil {
                        EmptyPreview()
                    }
                }

            VStack(spacing: 0) {
                TopBar(viewModel: viewModel)
                    .padding(.horizontal, 16)
                    .padding(.top, 12)

                Spacer(minLength: 20)

                ControlDock(viewModel: viewModel)
                    .padding(.horizontal, 14)
                    .padding(.bottom, 14)
            }
        }
        .preferredColorScheme(.dark)
    }
}

private struct TopBar: View {
    @ObservedObject var viewModel: StreamingViewModel

    var body: some View {
        HStack(spacing: 10) {
            VStack(alignment: .leading, spacing: 2) {
                Text("Cams")
                    .font(.system(size: 24, weight: .bold))
                Text(viewModel.localIPAddress == "-" ? "Local network pending" : "\(viewModel.localIPAddress):\(viewModel.localVideoPort)")
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.white.opacity(0.72))
            }

            Spacer()

            StatusPill(title: viewModel.connectionTitle, message: viewModel.statusMessage, tint: viewModel.statusTint)
        }
    }
}

private struct ControlDock: View {
    @ObservedObject var viewModel: StreamingViewModel

    var body: some View {
        VStack(spacing: 14) {
            HStack(spacing: 10) {
                MetricTile(title: "Quality", value: viewModel.selectedQuality.title, footnote: "\(viewModel.selectedQuality.detail) fps")
                MetricTile(title: "Bitrate", value: bitrateText, footnote: "Mbps")
                MetricTile(title: "Control", value: "\(viewModel.localControlPort)", footnote: "UDP")
            }

            Picker("Quality", selection: $viewModel.selectedQuality) {
                ForEach(StreamQuality.allCases) { quality in
                    Text(quality.title).tag(quality)
                }
            }
            .pickerStyle(.segmented)
            .onChange(of: viewModel.selectedQuality) { quality in
                viewModel.applyQuality(quality)
            }

            HStack(spacing: 10) {
                PrimaryStreamButton(
                    isStreaming: viewModel.isStreaming,
                    isStarting: viewModel.isStarting,
                    action: {
                        if viewModel.isStreaming || viewModel.isStarting {
                            viewModel.stopStreaming()
                        } else {
                            viewModel.startStreaming()
                        }
                    }
                )

                IconToggleButton(systemImage: "scope", title: "Focus", isActive: viewModel.focusLocked) {
                    viewModel.toggleFocusLock()
                }

                IconToggleButton(systemImage: "sun.max.fill", title: "Exposure", isActive: viewModel.exposureLocked) {
                    viewModel.toggleExposureLock()
                }

            }
        }
        .padding(14)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 24, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: 24, style: .continuous)
                .stroke(.white.opacity(0.12), lineWidth: 1)
        )
    }

    private var bitrateText: String {
        guard let mbps = viewModel.activeBitrateMbps else { return "-" }
        return mbps >= 10 ? String(format: "%.0f", mbps) : String(format: "%.1f", mbps)
    }
}

private struct PrimaryStreamButton: View {
    let isStreaming: Bool
    let isStarting: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            VStack(spacing: 6) {
                Image(systemName: iconName)
                    .font(.system(size: 24, weight: .semibold))
                Text(title)
                    .font(.caption.weight(.semibold))
                    .lineLimit(1)
                    .minimumScaleFactor(0.75)
            }
            .frame(maxWidth: .infinity, minHeight: 62)
            .background(background, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
            .foregroundStyle(.white)
        }
        .buttonStyle(.plain)
    }

    private var title: String {
        if isStarting { return "Starting" }
        return isStreaming ? "Stop" : "Start"
    }

    private var iconName: String {
        if isStarting { return "dot.radiowaves.left.and.right" }
        return isStreaming ? "stop.fill" : "video.fill"
    }

    private var background: Color {
        isStreaming || isStarting ? .red : .blue
    }
}

private struct IconToggleButton: View {
    let systemImage: String
    let title: String
    let isActive: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            VStack(spacing: 6) {
                Image(systemName: systemImage)
                    .font(.system(size: 21, weight: .semibold))
                Text(title)
                    .font(.caption2.weight(.semibold))
                    .lineLimit(1)
                    .minimumScaleFactor(0.7)
            }
            .frame(width: 68, height: 62)
            .background(isActive ? Color.orange : Color.white.opacity(0.13), in: RoundedRectangle(cornerRadius: 16, style: .continuous))
            .foregroundStyle(isActive ? .white : .white.opacity(0.88))
        }
        .buttonStyle(.plain)
    }
}

private struct MetricTile: View {
    let title: String
    let value: String
    let footnote: String

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title)
                .font(.caption2.weight(.semibold))
                .foregroundStyle(.white.opacity(0.58))
            HStack(alignment: .firstTextBaseline, spacing: 4) {
                Text(value)
                    .font(.system(size: 19, weight: .bold, design: .rounded))
                    .lineLimit(1)
                    .minimumScaleFactor(0.7)
                Text(footnote)
                    .font(.caption2.weight(.medium))
                    .foregroundStyle(.white.opacity(0.58))
                    .lineLimit(1)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, 10)
        .padding(.vertical, 9)
        .background(.black.opacity(0.25), in: RoundedRectangle(cornerRadius: 14, style: .continuous))
    }
}

private struct StatusPill: View {
    let title: String
    let message: String
    let tint: Color

    var body: some View {
        HStack(spacing: 8) {
            Circle()
                .fill(tint)
                .frame(width: 9, height: 9)
            VStack(alignment: .leading, spacing: 1) {
                Text(title)
                    .font(.caption.weight(.bold))
                    .lineLimit(1)
                Text(message)
                    .font(.caption2)
                    .foregroundStyle(.white.opacity(0.68))
                    .lineLimit(1)
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 9)
        .background(.black.opacity(0.42), in: Capsule())
        .overlay(Capsule().stroke(.white.opacity(0.12), lineWidth: 1))
        .frame(maxWidth: 230, alignment: .trailing)
    }
}

private struct EmptyPreview: View {
    var body: some View {
        LinearGradient(
            colors: [.black, Color(red: 0.06, green: 0.07, blue: 0.08), Color(red: 0.02, green: 0.03, blue: 0.04)],
            startPoint: .topLeading,
            endPoint: .bottomTrailing
        )
        .overlay {
            VStack(spacing: 14) {
                Image(systemName: "camera.aperture")
                    .font(.system(size: 54, weight: .light))
                    .foregroundStyle(.white.opacity(0.82))
                Text("Ready for OBS")
                    .font(.title3.weight(.semibold))
                    .foregroundStyle(.white.opacity(0.86))
            }
        }
    }
}

private struct CameraPreviewView: UIViewRepresentable {
    final class PreviewView: UIView {
        override class var layerClass: AnyClass { AVCaptureVideoPreviewLayer.self }
        var previewLayer: AVCaptureVideoPreviewLayer { layer as! AVCaptureVideoPreviewLayer }
    }

    let session: AVCaptureSession?

    func makeUIView(context: Context) -> PreviewView {
        let view = PreviewView()
        view.backgroundColor = .black
        view.previewLayer.videoGravity = .resizeAspectFill
        view.previewLayer.session = session
        return view
    }

    func updateUIView(_ uiView: PreviewView, context: Context) {
        uiView.previewLayer.session = session
    }
}

#Preview {
    ContentView()
}

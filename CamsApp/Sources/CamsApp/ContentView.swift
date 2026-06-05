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

    // ── Telemetry ─────────────────────────────────────────────────────────
    @Published var outgoingBitrateMbps: Double = 0.0
    @Published var packetDropsPerSec: Int = 0
    @Published var thermalTierLabel: String = "Nominal"
    @Published var centerStageEnabled: Bool = false

    // Rolling byte counter: accumulated in nonisolated callbacks, flushed by 1-sec timer.
    private var byteAccumulator: Int = 0
    private var dropAccumulator: Int = 0
    private var bitrateTimer: Timer?

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
        let csEngine = CenterStageEngine()
        csEngine.configure(
            encoderWidth: selectedQuality.encoderConfig.width,
            encoderHeight: selectedQuality.encoderConfig.height
        )
        session.centerStageEngine = csEngine
        capture = session
        previewSession = session.previewSession

        startBitrateTimer()

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
        bitrateTimer?.invalidate()
        bitrateTimer = nil
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
        outgoingBitrateMbps = 0.0
        packetDropsPerSec = 0
        thermalTierLabel = "Nominal"
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

    func toggleCenterStage() {
        centerStageEnabled.toggle()
        capture?.setCenterStageEnabled(centerStageEnabled)
        statusMessage = centerStageEnabled ? "Center Stage on" : "Center Stage off"
    }

    private func startBitrateTimer() {
        bitrateTimer?.invalidate()
        bitrateTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            guard let self else { return }
            Task { @MainActor [weak self] in
                guard let self else { return }
                self.outgoingBitrateMbps = Double(self.byteAccumulator) * 8.0 / 1_000_000.0
                self.packetDropsPerSec = self.dropAccumulator
                self.byteAccumulator = 0
                self.dropAccumulator = 0
                if let session = self.capture {
                    self.thermalTierLabel = session.thermalTierLabel
                }
            }
        }
    }

    private func handleStartupFailure(_ error: Error, session: CaptureSession) {
        bitrateTimer?.invalidate()
        bitrateTimer = nil
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
            self.dropAccumulator += 1
            self.statusMessage = "Network pressure; bitrate reduced"
        }
    }

    nonisolated func captureSession(_ session: CaptureSession, didSendBytes byteCount: Int) {
        Task { @MainActor in
            guard self.capture === session else { return }
            self.byteAccumulator += byteCount
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

    nonisolated func controlChannel(_ channel: ControlChannel, didSetCenterStageEnabled enabled: Bool) {
        Task { @MainActor in
            self.centerStageEnabled = enabled
            self.capture?.setCenterStageEnabled(enabled)
            self.statusMessage = enabled ? "Center Stage on (OBS)" : "Center Stage off (OBS)"
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

// MARK: - Design Tokens

private extension Color {
    // Accent palette
    static let camsEmerald = Color(red: 0,     green: 0.902, blue: 0.224)
    static let camsGarnet  = Color(red: 0.843, green: 0.231, blue: 0)
    static let camsAmber   = Color(red: 0.996, green: 0.702, blue: 0)

    // Surface stack — #0e0e0e → #131313 → #1c1b1b → #2a2a2a
    static let camsBg       = Color(red: 0.055, green: 0.055, blue: 0.055)
    static let camsTile     = Color(red: 0.075, green: 0.075, blue: 0.075)
    static let camsPanel    = Color(red: 0.110, green: 0.106, blue: 0.106)
    static let camsRaised   = Color(red: 0.165, green: 0.165, blue: 0.165)

    // Borders & recessed
    static let camsHairline = Color(red: 0.208, green: 0.208, blue: 0.204)   // #353534
    static let camsBorder   = Color(red: 0.267, green: 0.278, blue: 0.282)   // #444748
    static let camsRecessed = Color(red: 0.039, green: 0.039, blue: 0.039)   // #0a0a0a

    // Text
    static let camsTextPrimary   = Color(red: 0.898, green: 0.886, blue: 0.882)  // #e5e2e1
    static let camsTextSecondary = Color(red: 0.769, green: 0.780, blue: 0.784)  // #c4c7c8
    static let camsTextMuted     = Color(red: 0.557, green: 0.569, blue: 0.573)  // #8e9192
}

// MARK: - LED Indicator

private struct LEDIndicator: View {
    enum LEDColor { case emerald, amber, garnet, gray }
    let color: LEDColor

    var body: some View {
        Circle()
            .fill(fill)
            .frame(width: 7, height: 7)
            .shadow(color: glow.opacity(0.75), radius: 4)
    }

    private var fill: Color {
        switch color {
        case .emerald: return .camsEmerald
        case .amber:   return .camsAmber
        case .garnet:  return .camsGarnet
        case .gray:    return .camsBorder
        }
    }

    private var glow: Color {
        switch color {
        case .emerald: return .camsEmerald
        case .amber:   return .camsAmber
        case .garnet:  return .camsGarnet
        case .gray:    return .clear
        }
    }
}

// MARK: - Scanline Overlay

private struct ScanlineOverlay: View {
    var body: some View {
        Canvas { context, size in
            var y: CGFloat = 3
            while y < size.height {
                context.fill(
                    Path(CGRect(x: 0, y: y, width: size.width, height: 1)),
                    with: .color(.black.opacity(0.04))
                )
                y += 4
            }
        }
        .allowsHitTesting(false)
    }
}

// MARK: - Crosshair Reticle

private struct CrosshairReticle: View {
    var color: Color = Color.white.opacity(0.22)

    var body: some View {
        Canvas { context, size in
            let cx = size.width / 2
            let cy = size.height / 2
            let r: CGFloat = 40
            let arm: CGFloat = 12

            func drawCorner(x: CGFloat, y: CGFloat, dx: CGFloat, dy: CGFloat) {
                var p = Path()
                p.move(to: CGPoint(x: x, y: y + dy * arm))
                p.addLine(to: CGPoint(x: x, y: y))
                p.addLine(to: CGPoint(x: x + dx * arm, y: y))
                context.stroke(p, with: .color(color), lineWidth: 1)
            }

            drawCorner(x: cx - r, y: cy - r, dx: +1, dy: +1)
            drawCorner(x: cx + r, y: cy - r, dx: -1, dy: +1)
            drawCorner(x: cx - r, y: cy + r, dx: +1, dy: -1)
            drawCorner(x: cx + r, y: cy + r, dx: -1, dy: -1)
        }
        .allowsHitTesting(false)
    }
}

// MARK: - Viewfinder Vignette

private struct ViewfinderVignette: View {
    let isCritical: Bool
    let hasDrops: Bool

    var body: some View {
        Group {
            if isCritical {
                RadialGradient(
                    gradient: Gradient(colors: [.clear, Color.camsGarnet.opacity(0.22)]),
                    center: .center, startRadius: 100, endRadius: 420
                )
            } else if hasDrops {
                RadialGradient(
                    gradient: Gradient(colors: [.clear, Color.camsAmber.opacity(0.16)]),
                    center: .center, startRadius: 100, endRadius: 420
                )
            }
        }
        .allowsHitTesting(false)
    }
}

// MARK: - ContentView

struct ContentView: View {
    @StateObject private var viewModel = StreamingViewModel()
    @State private var thermalAcknowledged = false

    var isCritical: Bool { viewModel.isStreaming && viewModel.thermalTierLabel == "Critical" }
    var hasDrops: Bool { viewModel.isStreaming && viewModel.packetDropsPerSec > 0 }

    var body: some View {
        ZStack {
            // Camera feed or empty state
            if viewModel.previewSession != nil {
                CameraPreviewView(session: viewModel.previewSession)
                    .ignoresSafeArea()
            } else {
                EmptyPreview()
                    .ignoresSafeArea()
            }

            // Viewfinder FX (no hit-testing)
            ScanlineOverlay().ignoresSafeArea()
            CrosshairReticle()
            ViewfinderVignette(isCritical: isCritical, hasDrops: hasDrops)
                .ignoresSafeArea()
                .animation(.easeInOut(duration: 0.5), value: isCritical)
                .animation(.easeInOut(duration: 0.5), value: hasDrops)

            // Main UI overlay
            VStack(spacing: 0) {
                TopBar(viewModel: viewModel)
                    .padding(.horizontal, 16)
                    .padding(.top, 12)

                if hasDrops {
                    NetworkWarningBanner(drops: viewModel.packetDropsPerSec)
                        .padding(.horizontal, 16)
                        .padding(.top, 8)
                        .transition(.opacity.combined(with: .move(edge: .top)))
                }

                if viewModel.isStreaming {
                    TelemetryHUD(viewModel: viewModel)
                        .padding(.horizontal, 16)
                        .padding(.top, 8)
                        .transition(.opacity.combined(with: .move(edge: .top)))
                }

                Spacer(minLength: 20)

                ControlDock(viewModel: viewModel)
                    .padding(.horizontal, 14)
                    .padding(.bottom, 14)
            }
            .animation(.easeInOut(duration: 0.25), value: viewModel.isStreaming)
            .animation(.easeInOut(duration: 0.25), value: hasDrops)

            // Thermal critical modal
            if isCritical && !thermalAcknowledged {
                ThermalCriticalModal(viewModel: viewModel, acknowledged: $thermalAcknowledged)
                    .transition(.opacity)
            }
        }
        .preferredColorScheme(.dark)
        .onChange(of: viewModel.isStreaming) { streaming in
            if !streaming { thermalAcknowledged = false }
        }
        .onChange(of: viewModel.thermalTierLabel) { label in
            if label != "Critical" { thermalAcknowledged = false }
        }
    }
}

// MARK: - TopBar

private struct TopBar: View {
    @ObservedObject var viewModel: StreamingViewModel

    var body: some View {
        HStack(spacing: 10) {
            VStack(alignment: .leading, spacing: 3) {
                Text("CAMS")
                    .font(.system(size: 16, weight: .bold, design: .monospaced))
                    .foregroundStyle(Color.camsTextPrimary)
                    .tracking(3)
                Text(ipPortText)
                    .font(.system(size: 10, weight: .medium, design: .monospaced))
                    .foregroundStyle(Color.camsTextMuted)
            }

            Spacer()

            StatusPill(viewModel: viewModel)
        }
    }

    private var ipPortText: String {
        let ip = viewModel.localIPAddress
        if ip == "-" { return "PENDING · \(viewModel.localVideoPort)" }
        return "\(ip) · \(viewModel.localVideoPort)"
    }
}

// MARK: - StatusPill

private struct StatusPill: View {
    @ObservedObject var viewModel: StreamingViewModel

    var body: some View {
        HStack(spacing: 8) {
            LEDIndicator(color: ledColor)
            VStack(alignment: .leading, spacing: 1) {
                Text(viewModel.connectionTitle.uppercased())
                    .font(.system(size: 10, weight: .bold, design: .monospaced))
                    .foregroundStyle(Color.camsTextPrimary)
                    .lineLimit(1)
                Text(viewModel.statusMessage)
                    .font(.system(size: 9, weight: .medium, design: .monospaced))
                    .foregroundStyle(Color.camsTextMuted)
                    .lineLimit(1)
            }
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 8)
        .background(Color.camsTile)
        .clipShape(RoundedRectangle(cornerRadius: 4))
        .overlay(
            RoundedRectangle(cornerRadius: 4)
                .stroke(Color.camsHairline, lineWidth: 0.5)
        )
        .frame(maxWidth: 230, alignment: .trailing)
    }

    private var ledColor: LEDIndicator.LEDColor {
        if viewModel.isOBSConnected { return .emerald }
        if viewModel.isStarting || viewModel.isStreaming { return .amber }
        return .gray
    }
}

// MARK: - Network Warning Banner

private struct NetworkWarningBanner: View {
    let drops: Int

    var body: some View {
        HStack(spacing: 0) {
            Rectangle()
                .fill(Color.camsAmber)
                .frame(width: 3)

            HStack(spacing: 10) {
                LEDIndicator(color: .amber)
                VStack(alignment: .leading, spacing: 2) {
                    Text("NETWORK PRESSURE")
                        .font(.system(size: 10, weight: .bold, design: .monospaced))
                        .foregroundStyle(Color.camsAmber)
                        .tracking(1)
                    Text("\(drops) drops/sec · bitrate auto-reduced")
                        .font(.system(size: 9, weight: .medium, design: .monospaced))
                        .foregroundStyle(Color.camsTextSecondary)
                }
                Spacer()
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 10)
        }
        .background(Color.camsTile)
        .clipShape(RoundedRectangle(cornerRadius: 4))
        .overlay(
            RoundedRectangle(cornerRadius: 4)
                .stroke(Color.camsAmber.opacity(0.35), lineWidth: 0.5)
        )
    }
}

// MARK: - TelemetryHUD

private struct TelemetryHUD: View {
    @ObservedObject var viewModel: StreamingViewModel

    var body: some View {
        HStack(spacing: 0) {
            TelemetryCell(label: "BITRATE", value: bitrateText, unit: "Mbps")
            hairlineV
            TelemetryCell(
                label: "DROPS",
                value: "\(viewModel.packetDropsPerSec)",
                unit: "/sec",
                accent: viewModel.packetDropsPerSec > 0 ? Color.camsAmber : nil
            )
            hairlineV
            TelemetryCell(
                label: "THERMAL",
                value: viewModel.thermalTierLabel.uppercased(),
                unit: "",
                accent: thermalAccent
            )
        }
        .frame(maxWidth: .infinity)
        .frame(height: 52)
        .background(Color.camsPanel.opacity(0.92))
        .clipShape(RoundedRectangle(cornerRadius: 4))
        .overlay(
            RoundedRectangle(cornerRadius: 4)
                .stroke(Color.camsHairline, lineWidth: 0.5)
        )
    }

    private var hairlineV: some View {
        Rectangle()
            .fill(Color.camsHairline)
            .frame(width: 0.5)
            .padding(.vertical, 10)
    }

    private var bitrateText: String {
        let mbps = viewModel.outgoingBitrateMbps
        return mbps >= 10 ? String(format: "%.0f", mbps) : String(format: "%.1f", mbps)
    }

    private var thermalAccent: Color? {
        switch viewModel.thermalTierLabel {
        case "Serious":  return .camsAmber
        case "Critical": return .camsGarnet
        default:         return nil
        }
    }
}

private struct TelemetryCell: View {
    let label: String
    let value: String
    let unit: String
    var accent: Color? = nil

    var body: some View {
        VStack(spacing: 3) {
            Text(label)
                .font(.system(size: 8, weight: .bold, design: .monospaced))
                .foregroundStyle(Color.camsTextMuted)
                .tracking(1.5)
            HStack(alignment: .firstTextBaseline, spacing: 3) {
                Text(value)
                    .font(.system(size: 14, weight: .semibold, design: .monospaced))
                    .foregroundStyle(accent ?? Color.camsTextPrimary)
                    .lineLimit(1)
                    .minimumScaleFactor(0.8)
                if !unit.isEmpty {
                    Text(unit)
                        .font(.system(size: 9, weight: .medium, design: .monospaced))
                        .foregroundStyle(Color.camsTextMuted)
                }
            }
        }
        .frame(maxWidth: .infinity)
    }
}

// MARK: - ControlDock

private struct ControlDock: View {
    @ObservedObject var viewModel: StreamingViewModel

    var body: some View {
        VStack(spacing: 0) {
            // Metric tiles
            HStack(spacing: 0) {
                MetricTile(label: "QUALITY", value: viewModel.selectedQuality.title,
                           footnote: "\(viewModel.selectedQuality.detail) FPS")
                hairlineV.padding(.vertical, 10)
                MetricTile(label: "BITRATE", value: bitrateText, footnote: "Mbps")
                hairlineV.padding(.vertical, 10)
                MetricTile(label: "CTRL", value: "\(viewModel.localControlPort)", footnote: "UDP")
            }
            .frame(height: 56)

            hairlineH

            // Quality picker
            QualityPicker(viewModel: viewModel)
                .padding(.horizontal, 12)
                .padding(.vertical, 10)

            hairlineH

            // Button row
            HStack(spacing: 0) {
                PrimaryStreamButton(viewModel: viewModel)
                hairlineV
                IconToggleButton(
                    systemImage: "scope", label: "FOCUS",
                    isActive: viewModel.focusLocked, activeColor: .amber
                ) { viewModel.toggleFocusLock() }
                .frame(width: 72)
                hairlineV
                IconToggleButton(
                    systemImage: "sun.max.fill", label: "EXPOSE",
                    isActive: viewModel.exposureLocked, activeColor: .amber
                ) { viewModel.toggleExposureLock() }
                .frame(width: 72)
                hairlineV
                IconToggleButton(
                    systemImage: "person.crop.rectangle.fill", label: "TRACK",
                    isActive: viewModel.centerStageEnabled, activeColor: .emerald
                ) { viewModel.toggleCenterStage() }
                .frame(width: 72)
            }
            .frame(height: 72)
        }
        .background(Color.camsPanel)
        .clipShape(RoundedRectangle(cornerRadius: 4))
        .overlay(
            RoundedRectangle(cornerRadius: 4)
                .stroke(Color.camsHairline, lineWidth: 0.5)
        )
    }

    private var hairlineH: some View {
        Rectangle().fill(Color.camsHairline).frame(height: 0.5)
    }

    private var hairlineV: some View {
        Rectangle().fill(Color.camsHairline).frame(width: 0.5).frame(maxHeight: .infinity)
    }

    private var bitrateText: String {
        guard let mbps = viewModel.activeBitrateMbps else { return "—" }
        return mbps >= 10 ? String(format: "%.0f", mbps) : String(format: "%.1f", mbps)
    }
}

// MARK: - MetricTile

private struct MetricTile: View {
    let label: String
    let value: String
    let footnote: String

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(label)
                .font(.system(size: 8, weight: .bold, design: .monospaced))
                .foregroundStyle(Color.camsTextMuted)
                .tracking(1)
            HStack(alignment: .firstTextBaseline, spacing: 3) {
                Text(value)
                    .font(.system(size: 18, weight: .bold, design: .monospaced))
                    .foregroundStyle(Color.camsTextPrimary)
                    .lineLimit(1)
                    .minimumScaleFactor(0.7)
                Text(footnote)
                    .font(.system(size: 9, weight: .medium, design: .monospaced))
                    .foregroundStyle(Color.camsTextMuted)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, 12)
        .padding(.vertical, 10)
    }
}

// MARK: - Quality Picker

private struct QualityPicker: View {
    @ObservedObject var viewModel: StreamingViewModel

    var body: some View {
        HStack(spacing: 0) {
            ForEach(Array(StreamQuality.allCases.enumerated()), id: \.element.id) { index, quality in
                if index > 0 {
                    Rectangle()
                        .fill(Color.camsHairline)
                        .frame(width: 0.5)
                        .frame(maxHeight: .infinity)
                }
                Button {
                    viewModel.applyQuality(quality)
                } label: {
                    VStack(spacing: 2) {
                        Text(quality.title)
                            .font(.system(size: 12, weight: .bold, design: .monospaced))
                        Text("\(quality.detail) fps")
                            .font(.system(size: 9, weight: .medium, design: .monospaced))
                            .foregroundStyle(
                                viewModel.selectedQuality == quality
                                    ? Color.camsTextMuted
                                    : Color.camsTextMuted.opacity(0.6)
                            )
                    }
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 7)
                    .background(
                        viewModel.selectedQuality == quality
                            ? Color.camsRaised
                            : Color.clear
                    )
                    .foregroundStyle(
                        viewModel.selectedQuality == quality
                            ? Color.camsTextPrimary
                            : Color.camsTextMuted
                    )
                }
                .buttonStyle(.plain)
            }
        }
        .frame(height: 36)
        .clipShape(RoundedRectangle(cornerRadius: 2))
        .overlay(
            RoundedRectangle(cornerRadius: 2)
                .stroke(Color.camsHairline, lineWidth: 0.5)
        )
    }
}

// MARK: - Primary Stream Button

private struct PrimaryStreamButton: View {
    @ObservedObject var viewModel: StreamingViewModel

    var body: some View {
        Button {
            if viewModel.isStreaming || viewModel.isStarting {
                viewModel.stopStreaming()
            } else {
                viewModel.startStreaming()
            }
        } label: {
            VStack(spacing: 5) {
                Image(systemName: iconName)
                    .font(.system(size: 20, weight: .semibold))
                Text(title)
                    .font(.system(size: 9, weight: .bold, design: .monospaced))
                    .tracking(2)
                    .lineLimit(1)
            }
            .foregroundStyle(Color.camsTextPrimary)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .buttonStyle(StreamButtonStyle(isDestructive: viewModel.isStreaming || viewModel.isStarting))
    }

    private var title: String {
        if viewModel.isStarting { return "STARTING" }
        return viewModel.isStreaming ? "STOP" : "START"
    }

    private var iconName: String {
        if viewModel.isStarting { return "dot.radiowaves.left.and.right" }
        return viewModel.isStreaming ? "stop.fill" : "video.fill"
    }
}

private struct StreamButtonStyle: ButtonStyle {
    let isDestructive: Bool

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .background(
                Group {
                    if isDestructive {
                        LinearGradient(
                            colors: [
                                Color(red: 0.55, green: 0.13, blue: 0),
                                Color(red: 0.42, green: 0.08, blue: 0)
                            ],
                            startPoint: .top, endPoint: .bottom
                        )
                    } else {
                        LinearGradient(
                            colors: [Color(white: 0.14), Color(white: 0.09)],
                            startPoint: .top, endPoint: .bottom
                        )
                    }
                }
            )
            .overlay(
                Rectangle().stroke(
                    isDestructive ? Color.camsGarnet.opacity(0.5) : Color(white: 0.20),
                    lineWidth: 0.5
                )
            )
            .opacity(configuration.isPressed ? 0.72 : 1.0)
            .scaleEffect(configuration.isPressed ? 0.98 : 1.0)
            .animation(.easeInOut(duration: 0.08), value: configuration.isPressed)
    }
}

// MARK: - Icon Toggle Button

private struct IconToggleButton: View {
    enum ActiveColor { case amber, emerald }

    let systemImage: String
    let label: String
    let isActive: Bool
    let activeColor: ActiveColor
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            VStack(spacing: 5) {
                Image(systemName: systemImage)
                    .font(.system(size: 17, weight: .semibold))
                    .foregroundStyle(iconColor)
                Text(label)
                    .font(.system(size: 8, weight: .bold, design: .monospaced))
                    .foregroundStyle(labelColor)
                    .tracking(1)
                    .lineLimit(1)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(
                Group {
                    if isActive {
                        Color.camsRecessed
                    } else {
                        LinearGradient(
                            colors: [Color(white: 0.14), Color(white: 0.09)],
                            startPoint: .top, endPoint: .bottom
                        )
                    }
                }
            )
            .overlay(
                Rectangle().stroke(
                    isActive ? activeBorderColor.opacity(0.85) : Color(white: 0.20),
                    lineWidth: isActive ? 1 : 0.5
                )
                .shadow(color: isActive ? activeBorderColor.opacity(0.25) : .clear, radius: 6)
            )
        }
        .buttonStyle(.plain)
    }

    private var activeBorderColor: Color {
        activeColor == .amber ? .camsAmber : .camsEmerald
    }

    private var iconColor: Color {
        isActive ? activeBorderColor : Color.camsTextSecondary
    }

    private var labelColor: Color {
        isActive ? activeBorderColor : Color.camsTextMuted
    }
}

// MARK: - Empty Preview

private struct EmptyPreview: View {
    var body: some View {
        ZStack {
            Color.camsBg

            ScanlineOverlay()

            CrosshairReticle(color: Color.camsHairline)

            VStack(spacing: 16) {
                Image(systemName: "camera.aperture")
                    .font(.system(size: 52, weight: .ultraLight))
                    .foregroundStyle(Color.camsTextMuted)

                VStack(spacing: 4) {
                    Text("NO FEED")
                        .font(.system(size: 13, weight: .bold, design: .monospaced))
                        .foregroundStyle(Color.camsTextSecondary)
                        .tracking(4)
                    Text("READY FOR OBS")
                        .font(.system(size: 10, weight: .medium, design: .monospaced))
                        .foregroundStyle(Color.camsTextMuted)
                        .tracking(2.5)
                }
            }
        }
    }
}

// MARK: - Thermal Critical Modal

private struct ThermalCriticalModal: View {
    @ObservedObject var viewModel: StreamingViewModel
    @Binding var acknowledged: Bool

    var body: some View {
        ZStack {
            Color.black.opacity(0.70)
                .ignoresSafeArea()

            VStack(spacing: 0) {
                // Header row
                HStack(spacing: 10) {
                    LEDIndicator(color: .garnet)
                    Text("THERMAL CRITICAL")
                        .font(.system(size: 12, weight: .bold, design: .monospaced))
                        .foregroundStyle(Color.camsGarnet)
                        .tracking(1.5)
                    Spacer()
                }
                .padding(.horizontal, 16)
                .padding(.vertical, 12)
                .background(Color.camsGarnet.opacity(0.10))

                Rectangle().fill(Color.camsGarnet.opacity(0.35)).frame(height: 0.5)

                // Body
                VStack(alignment: .leading, spacing: 12) {
                    Text("Device temperature is critical. Video quality has been automatically reduced to protect hardware.")
                        .font(.system(size: 12, weight: .regular))
                        .foregroundStyle(Color.camsTextSecondary)
                        .lineSpacing(4)
                        .fixedSize(horizontal: false, vertical: true)

                    // Quality readout
                    HStack {
                        Text("AUTO-REDUCED TO")
                            .font(.system(size: 8, weight: .bold, design: .monospaced))
                            .foregroundStyle(Color.camsTextMuted)
                            .tracking(1)
                        Spacer()
                        Text("\(viewModel.selectedQuality.title) · \(bitrateText) Mbps")
                            .font(.system(size: 11, weight: .bold, design: .monospaced))
                            .foregroundStyle(Color.camsGarnet)
                    }
                    .padding(.horizontal, 10)
                    .padding(.vertical, 8)
                    .background(Color.camsRecessed)
                    .clipShape(RoundedRectangle(cornerRadius: 2))
                    .overlay(
                        RoundedRectangle(cornerRadius: 2)
                            .stroke(Color.camsGarnet.opacity(0.3), lineWidth: 0.5)
                    )
                }
                .padding(16)

                Rectangle().fill(Color.camsHairline).frame(height: 0.5)

                // Acknowledge button
                Button {
                    acknowledged = true
                } label: {
                    Text("ACKNOWLEDGED")
                        .font(.system(size: 11, weight: .bold, design: .monospaced))
                        .foregroundStyle(Color.camsTextPrimary)
                        .tracking(2)
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 14)
                }
                .buttonStyle(.plain)
                .background(
                    LinearGradient(
                        colors: [Color(white: 0.14), Color(white: 0.09)],
                        startPoint: .top, endPoint: .bottom
                    )
                )
            }
            .background(Color.camsPanel)
            .clipShape(RoundedRectangle(cornerRadius: 4))
            .overlay(
                RoundedRectangle(cornerRadius: 4)
                    .stroke(Color.camsGarnet.opacity(0.45), lineWidth: 0.5)
            )
            .padding(.horizontal, 32)
        }
    }

    private var bitrateText: String {
        guard let mbps = viewModel.activeBitrateMbps else { return "—" }
        return mbps >= 10 ? String(format: "%.0f", mbps) : String(format: "%.1f", mbps)
    }
}

// MARK: - Camera Preview

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

// swift-tools-version: 5.9
// The swift-tools-version declares the minimum version of Swift required to build this package.
//
// Cams — iOS high-performance camera streaming application
// Targets iOS 16+ for full Network.framework and VideoToolbox HEVC support.

import PackageDescription

let package = Package(
    name: "CamsApp",
    platforms: [
        .iOS(.v16),
        .macOS(.v10_14)
    ],
    products: [
        .library(
            name: "CamsCore",
            targets: ["CamsCore"]
        )
    ],
    dependencies: [],
    targets: [
        // Core protocol/transport packetisation logic — importable for unit tests.
        .target(
            name: "CamsCore",
            dependencies: [],
            path: "Sources/CamsApp",
            exclude: [
                "BonjourPublisher.swift",
                "CamsApp.swift",
                "CaptureSession.swift",
                "CircularBufferPool.swift",
                "ContentView.swift",
                "ControlChannel.swift",
                "VideoEncoder.swift"
            ],
            sources: [
                "CamsProtocol.swift",
                "NetworkTransport.swift"
            ],
            swiftSettings: [
                // Enable strict concurrency checking to catch data-race issues at compile time.
                .enableExperimentalFeature("StrictConcurrency")
            ]
        ),
        .testTarget(
            name: "CamsCoreTests",
            dependencies: ["CamsCore"],
            path: "Tests/CamsCoreTests"
        )
    ]
)

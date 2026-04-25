// swift-tools-version: 5.9
// The swift-tools-version declares the minimum version of Swift required to build this package.
//
// Cams — iOS high-performance camera streaming application
// Targets iOS 16+ for full Network.framework and VideoToolbox HEVC support.

import PackageDescription

let package = Package(
    name: "CamsApp",
    platforms: [
        .iOS(.v16)
    ],
    products: [
        .library(
            name: "CamsCore",
            targets: ["CamsCore"]
        )
    ],
    dependencies: [],
    targets: [
        // Core library containing all streaming logic — importable for unit tests.
        .target(
            name: "CamsCore",
            dependencies: [],
            path: "Sources/CamsApp",
            sources: [
                "CamsProtocol.swift"
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

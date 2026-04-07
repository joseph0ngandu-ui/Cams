// CircularBufferPool.swift
// Cams — Zero-copy circular pixel buffer pool.
//
// This pool pre-allocates a fixed set of CVPixelBuffers and hands them to the
// AVCaptureVideoDataOutput callback.  When encoding is complete the buffer is
// returned to the pool rather than deallocated, eliminating per-frame heap
// allocation pressure on the critical capture path.
//
// Thread safety: all public methods are safe to call from any thread.

import CoreVideo
import Foundation

// MARK: - CircularBufferPool

/// A fixed-capacity pool of reusable `CVPixelBuffer` instances.
///
/// Callers obtain a buffer with `dequeue()`, use it, and then call `enqueue(_:)`
/// to return it. If the pool is empty when `dequeue()` is called the method
/// blocks for up to `timeout` seconds before returning `nil`, giving in-flight
/// buffers a chance to be returned.
public final class CircularBufferPool: @unchecked Sendable {

    // MARK: - Configuration

    /// Number of pre-allocated buffers in the pool.
    public let capacity: Int
    /// Pixel-buffer width in pixels.
    public let width: Int
    /// Pixel-buffer height in pixels.
    public let height: Int
    /// CoreVideo pixel format.  Defaults to `kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange`.
    public let pixelFormat: OSType

    // MARK: - Private state

    private let lock = NSLock()
    private let semaphore: DispatchSemaphore
    private var available: [CVPixelBuffer]

    // MARK: - Lifecycle

    /// Creates the pool and pre-allocates `capacity` pixel buffers.
    ///
    /// - Throws: `CircularBufferPoolError.allocationFailed` if any buffer cannot
    ///   be created (e.g., insufficient memory).
    public init(
        capacity: Int,
        width: Int,
        height: Int,
        pixelFormat: OSType = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
    ) throws {
        precondition(capacity > 0, "Pool capacity must be positive")
        precondition(width  > 0 && height > 0, "Dimensions must be positive")

        self.capacity    = capacity
        self.width       = width
        self.height      = height
        self.pixelFormat = pixelFormat
        self.semaphore   = DispatchSemaphore(value: 0)
        self.available   = []
        self.available.reserveCapacity(capacity)

        let attrs: [String: Any] = [
            kCVPixelBufferIOSurfacePropertiesKey as String: [:],
            kCVPixelBufferWidthKey              as String: width,
            kCVPixelBufferHeightKey             as String: height,
            kCVPixelBufferPixelFormatTypeKey    as String: pixelFormat
        ]

        for _ in 0..<capacity {
            var pixelBuffer: CVPixelBuffer?
            let status = CVPixelBufferCreate(
                kCFAllocatorDefault,
                width,
                height,
                pixelFormat,
                attrs as CFDictionary,
                &pixelBuffer
            )
            guard status == kCVReturnSuccess, let buffer = pixelBuffer else {
                throw CircularBufferPoolError.allocationFailed(status: status)
            }
            available.append(buffer)
            semaphore.signal()
        }
    }

    // MARK: - Public interface

    /// Dequeues a buffer from the pool, waiting up to `timeout` seconds.
    ///
    /// Returns `nil` if no buffer becomes available within the timeout.
    public func dequeue(timeout: DispatchTime = .now() + .milliseconds(33)) -> CVPixelBuffer? {
        guard semaphore.wait(timeout: timeout) == .success else { return nil }
        lock.lock()
        defer { lock.unlock() }
        return available.popLast()
    }

    /// Returns a buffer to the pool so it can be reused.
    ///
    /// The caller **must not** retain or use the buffer after calling this method.
    public func enqueue(_ buffer: CVPixelBuffer) {
        lock.lock()
        available.append(buffer)
        lock.unlock()
        semaphore.signal()
    }

    /// The number of buffers currently available for dequeue.
    public var availableCount: Int {
        lock.lock()
        defer { lock.unlock() }
        return available.count
    }
}

// MARK: - CircularBufferPoolError

public enum CircularBufferPoolError: Error, CustomStringConvertible {
    case allocationFailed(status: CVReturn)

    public var description: String {
        switch self {
        case .allocationFailed(let status):
            return "CVPixelBufferCreate failed with status \(status)"
        }
    }
}

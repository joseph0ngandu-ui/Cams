// CamsProtocolTests.swift
// Tests for CamsProtocol — packet header serialisation/deserialisation round-trips.

import XCTest
@testable import CamsCore

final class CamsProtocolTests: XCTestCase {

    // MARK: - CamsPacketHeader

    func testHeaderSerialiseDeserialiseRoundTrip() {
        let original = CamsPacketHeader(
            sequenceNumber: 42,
            timestamp: 1_234_567_890_123,
            frameType: .hevc,
            payloadLength: 512
        )

        let data = original.serialise()

        // Header must be exactly kCamsHeaderSize bytes.
        XCTAssertEqual(data.count, kCamsHeaderSize)

        guard let parsed = CamsPacketHeader.deserialise(from: data) else {
            XCTFail("Deserialisation returned nil")
            return
        }

        XCTAssertEqual(parsed.sequenceNumber, original.sequenceNumber)
        XCTAssertEqual(parsed.timestamp,      original.timestamp)
        XCTAssertEqual(parsed.frameType,      original.frameType)
        XCTAssertEqual(parsed.payloadLength,  original.payloadLength)
    }

    func testHeaderDeserialisationReturnsNilForShortData() {
        let shortData = Data(repeating: 0, count: kCamsHeaderSize - 1)
        XCTAssertNil(CamsPacketHeader.deserialise(from: shortData))
    }

    func testHeaderDeserialisationReturnsNilForUnknownFrameType() {
        var data = Data(repeating: 0, count: kCamsHeaderSize)
        // Write 0xAB at byte offset 12 (frame-type field) — an unrecognised type.
        data[12] = 0xAB
        XCTAssertNil(CamsPacketHeader.deserialise(from: data))
    }

    func testAllFrameTypesRoundTrip() {
        let types: [FrameType] = [.h264, .hevc, .parameterSets, .keyframe, .eos]
        for ft in types {
            let header = CamsPacketHeader(
                sequenceNumber: 0,
                timestamp: 0,
                frameType: ft,
                payloadLength: 0
            )
            let parsed = CamsPacketHeader.deserialise(from: header.serialise())
            XCTAssertNotNil(parsed, "Failed for frameType \(ft)")
            XCTAssertEqual(parsed?.frameType, ft)
        }
    }

    func testBigEndianEncoding() {
        // sequence 0x01020304 should appear as bytes [01, 02, 03, 04]
        let header = CamsPacketHeader(
            sequenceNumber: 0x01020304,
            timestamp: 0x0102030405060708,
            frameType: .h264,
            payloadLength: 0x0A0B0C0D
        )
        let data = header.serialise()

        XCTAssertEqual(data[0], 0x01)
        XCTAssertEqual(data[1], 0x02)
        XCTAssertEqual(data[2], 0x03)
        XCTAssertEqual(data[3], 0x04)
        // Timestamp bytes 4–11
        XCTAssertEqual(data[4], 0x01)
        XCTAssertEqual(data[11], 0x08)
        // Payload length bytes 13–16
        XCTAssertEqual(data[13], 0x0A)
        XCTAssertEqual(data[16], 0x0D)
    }

    // MARK: - CamsControlPacket

    func testControlPacketRoundTrip() {
        let original = CamsControlPacket(
            command: .lockFocus,
            payload: Data([0x01, 0x02, 0x03])
        )
        let data = original.serialise()
        guard let parsed = CamsControlPacket.deserialise(from: data) else {
            XCTFail("Control packet deserialisation returned nil")
            return
        }
        XCTAssertEqual(parsed.command, original.command)
        XCTAssertEqual(parsed.payload, original.payload)
    }

    func testControlPacketEmptyPayload() {
        let pkt = CamsControlPacket(command: .ping)
        let data = pkt.serialise()
        XCTAssertEqual(data.count, 1)
        let parsed = CamsControlPacket.deserialise(from: data)
        XCTAssertEqual(parsed?.command, .ping)
        XCTAssertEqual(parsed?.payload.count, 0)
    }

    func testControlPacketNilForEmptyData() {
        XCTAssertNil(CamsControlPacket.deserialise(from: Data()))
    }

    func testControlPacketNilForUnknownCommand() {
        let data = Data([0xEE])  // 0xEE is not a valid ControlCommand
        XCTAssertNil(CamsControlPacket.deserialise(from: data))
    }

    // MARK: - Timestamp

    func testTimestampIsMonotonicallyIncreasing() {
        var previous: UInt64 = 0
        for _ in 0..<100 {
            let now = camsCurrentTimestampNs()
            XCTAssertGreaterThanOrEqual(now, previous)
            previous = now
        }
    }

    // MARK: - FrameType helpers

    func testIsVideoDataFlag() {
        XCTAssertTrue(FrameType.h264.isVideoData)
        XCTAssertTrue(FrameType.hevc.isVideoData)
        XCTAssertTrue(FrameType.keyframe.isVideoData)
        XCTAssertTrue(FrameType.parameterSets.isVideoData)
        XCTAssertFalse(FrameType.eos.isVideoData)
    }
}

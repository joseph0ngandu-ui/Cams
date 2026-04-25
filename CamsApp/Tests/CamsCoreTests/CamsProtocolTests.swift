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

    func testHeaderFragmentFieldsRoundTrip() {
        let original = CamsPacketHeader(
            sequenceNumber: 7,
            timestamp: 99,
            frameType: .keyframe,
            fragmentIndex: 2,
            fragmentCount: 4,
            payloadLength: 1200
        )

        let parsed = CamsPacketHeader.deserialise(from: original.serialise())
        XCTAssertEqual(parsed?.fragmentIndex, 2)
        XCTAssertEqual(parsed?.fragmentCount, 4)
    }

    func testHeaderRejectsInvalidFragmentIndex() {
        let header = CamsPacketHeader(
            sequenceNumber: 7,
            timestamp: 99,
            frameType: .keyframe,
            fragmentIndex: 4,
            fragmentCount: 4,
            payloadLength: 1200
        )

        XCTAssertNil(CamsPacketHeader.deserialise(from: header.serialise()))
    }

    func testHeaderRejectsTooManyFragments() {
        var data = CamsPacketHeader(
            sequenceNumber: 7,
            timestamp: 99,
            frameType: .keyframe,
            fragmentIndex: 0,
            fragmentCount: 1,
            payloadLength: 1200
        ).serialise()

        let invalidCount = kCamsMaxFragmentsPerFrame + 1
        data[15] = UInt8((invalidCount >> 8) & 0xFF)
        data[16] = UInt8(invalidCount & 0xFF)

        XCTAssertNil(CamsPacketHeader.deserialise(from: data))
    }

    func testFullPacketDeserialisationValidatesPayloadLength() {
        let header = CamsPacketHeader(
            sequenceNumber: 1,
            timestamp: 2,
            frameType: .h264,
            payloadLength: 3
        )
        let valid = header.serialise() + Data([0xAA, 0xBB, 0xCC])
        let invalid = header.serialise() + Data([0xAA, 0xBB])

        XCTAssertEqual(CamsPacket.deserialise(from: valid)?.payload, Data([0xAA, 0xBB, 0xCC]))
        XCTAssertNil(CamsPacket.deserialise(from: invalid))
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
        // Fragment index (bytes 13–14) — should stay zero.
        XCTAssertEqual(data[13], 0x00)
        XCTAssertEqual(data[14], 0x00)
        // Fragment count (bytes 15–16) — default is 1.
        XCTAssertEqual(data[15], 0x00)
        XCTAssertEqual(data[16], 0x01)
        // Payload length bytes 17–20
        XCTAssertEqual(data[17], 0x0A)
        XCTAssertEqual(data[20], 0x0D)
        XCTAssertEqual(data[18], 0x0B)
        XCTAssertEqual(data[19], 0x0C)
    }

    // MARK: - Packetisation

    func testPacketizerProducesSingleFragmentForSmallFrame() throws {
        var packetizer = CamsFramePacketizer()
        let packets = try packetizer.packetise(
            data: Data([0x01, 0x02, 0x03]),
            frameType: .h264,
            timestamp: 123,
            fragmentPayloadSize: 1200
        )

        XCTAssertEqual(packets.count, 1)
        let packet = CamsPacket.deserialise(from: packets[0])
        XCTAssertEqual(packet?.header.sequenceNumber, 0)
        XCTAssertEqual(packet?.header.fragmentIndex, 0)
        XCTAssertEqual(packet?.header.fragmentCount, 1)
        XCTAssertEqual(packet?.payload, Data([0x01, 0x02, 0x03]))
    }

    func testPacketizerFragmentsLargeFrameWithOneFrameSequence() throws {
        var packetizer = CamsFramePacketizer()
        let payload = Data((0..<10).map(UInt8.init))
        let packets = try packetizer.packetise(
            data: payload,
            frameType: .hevc,
            timestamp: 456,
            fragmentPayloadSize: 4
        )

        XCTAssertEqual(packets.count, 3)
        let parsed = packets.compactMap { CamsPacket.deserialise(from: $0) }
        XCTAssertEqual(parsed.count, 3)
        XCTAssertEqual(Set(parsed.map { $0.header.sequenceNumber }), Set([UInt32(0)]))
        XCTAssertEqual(parsed.map { $0.header.fragmentIndex }, [UInt16(0), 1, 2])
        XCTAssertEqual(parsed.map { $0.header.fragmentCount }, [UInt16(3), 3, 3])
        XCTAssertEqual(parsed.reduce(Data()) { $0 + $1.payload }, payload)
    }

    func testPacketizerIncrementsSequencePerFrameNotPerFragment() throws {
        var packetizer = CamsFramePacketizer()
        let firstFrame = try packetizer.packetise(
            data: Data((0..<10).map(UInt8.init)),
            frameType: .h264,
            timestamp: 1,
            fragmentPayloadSize: 4
        )
        let secondFrame = try packetizer.packetise(
            data: Data([0xFF]),
            frameType: .h264,
            timestamp: 2,
            fragmentPayloadSize: 4
        )

        XCTAssertEqual(CamsPacket.deserialise(from: firstFrame[0])?.header.sequenceNumber, 0)
        XCTAssertEqual(CamsPacket.deserialise(from: firstFrame[2])?.header.sequenceNumber, 0)
        XCTAssertEqual(CamsPacket.deserialise(from: secondFrame[0])?.header.sequenceNumber, 1)
    }

    func testPacketizerRejectsFramesAboveFragmentLimit() {
        var packetizer = CamsFramePacketizer()
        let payload = Data(repeating: 0xAB, count: Int(kCamsMaxFragmentsPerFrame) + 1)

        XCTAssertThrowsError(
            try packetizer.packetise(
                data: payload,
                frameType: .h264,
                timestamp: 1,
                fragmentPayloadSize: 1
            )
        ) { error in
            XCTAssertEqual(
                error as? CamsPacketisationError,
                .tooManyFragments(
                    fragmentCount: Int(kCamsMaxFragmentsPerFrame) + 1,
                    maxFragments: Int(kCamsMaxFragmentsPerFrame)
                )
            )
        }
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

#include <AstrOsEspNowProtocol.hpp>
#include <AstrOsMessaging.hpp>
#include <PacketTracker.hpp>
#include <cstring>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

// M1 — Task 1: enum values + ENC string mapping for the 8 new OTA packet types.

TEST(OtaPacketTypes, EnumValuesPresent)
{
    // Compile-time enum checks. These will fail to compile if the enum is missing values.
    AstrOsPacketType types[] = {
        AstrOsPacketType::OTA_BEGIN, AstrOsPacketType::OTA_BEGIN_ACK, AstrOsPacketType::OTA_BEGIN_NAK,
        AstrOsPacketType::OTA_DATA,  AstrOsPacketType::OTA_DATA_ACK,  AstrOsPacketType::OTA_DATA_NAK,
        AstrOsPacketType::OTA_END,   AstrOsPacketType::OTA_END_ACK,
    };
    EXPECT_EQ(8u, sizeof(types) / sizeof(types[0]));
}

TEST(OtaPacketTypes, PacketTypeMapContainsOtaTypes)
{
    // The existing string-payload path (generatePackets) is NOT how OTA frames get built.
    // It calls packetTypeMap[type] and returns empty on unknown types. After Task 1 the
    // OTA types are in the map, so generatePackets returns a non-empty vector if we
    // (incorrectly) call the string path — but the resulting packet would have the
    // validator-string in the payload, which is wrong for OTA. Task 3 adds the proper
    // binary builder. This test just locks in: "Task 1 makes the map lookup succeed."
    auto svc = AstrOsEspNowMessageService();
    auto packets = svc.generatePackets(AstrOsPacketType::OTA_BEGIN, "ignored");
    ASSERT_EQ(1u, packets.size());
    // Cleanup
    for (auto &p : packets)
        free(p.data);
}

// M1 — Task 2: packed payload structs with byte-offset assertions.

TEST(OtaWirePayloads, OtaBeginPayloadMatchesLittleEndianByteLayout)
{
    OtaBeginPayload p{};
    p.xferId = 0x42;
    p.totalSize = 0x12345678;
    p.chunkSize = 128;
    p.totalChunks = 9600;
    for (int i = 0; i < 32; i++)
        p.sha256Expected[i] = static_cast<uint8_t>(i);
    p.flags = 0;

    // sizeof(OtaBeginPayload) is locked by static_assert in the header.
    // Here we just confirm that interpreting the struct as bytes preserves layout.
    uint8_t buf[sizeof(OtaBeginPayload)];
    std::memcpy(buf, &p, sizeof(p));

    EXPECT_EQ(0x42, buf[0]); // xferId
    // u32 little-endian at offset 1: 0x12345678 → 78 56 34 12
    EXPECT_EQ(0x78, buf[1]);
    EXPECT_EQ(0x56, buf[2]);
    EXPECT_EQ(0x34, buf[3]);
    EXPECT_EQ(0x12, buf[4]);
    // u16 little-endian at offset 5: 128 → 80 00
    EXPECT_EQ(0x80, buf[5]);
    EXPECT_EQ(0x00, buf[6]);
    // u32 little-endian at offset 7: 9600 → 80 25 00 00
    EXPECT_EQ(0x80, buf[7]);
    EXPECT_EQ(0x25, buf[8]);
    EXPECT_EQ(0x00, buf[9]);
    EXPECT_EQ(0x00, buf[10]);
    // u8[32] sha256 at offset 11..42
    for (int i = 0; i < 32; i++)
        EXPECT_EQ(static_cast<uint8_t>(i), buf[11 + i]);
    EXPECT_EQ(0u, buf[43]); // flags
}

TEST(OtaWirePayloads, OtaDataNakReasonValuesMatchSpec)
{
    // Spec freezes these values: CRC=1, SIZE=2, OUT_OF_ORDER=3, WRITE=4.
    // Reordering or renumbering breaks the wire contract.
    EXPECT_EQ(0, static_cast<int>(OtaDataNakReason::NONE));
    EXPECT_EQ(1, static_cast<int>(OtaDataNakReason::CRC));
    EXPECT_EQ(2, static_cast<int>(OtaDataNakReason::SIZE));
    EXPECT_EQ(3, static_cast<int>(OtaDataNakReason::OUT_OF_ORDER));
    EXPECT_EQ(4, static_cast<int>(OtaDataNakReason::WRITE));
}

TEST(OtaWirePayloads, OtaBeginNakReasonValuesMatchSpec)
{
    EXPECT_EQ(0, static_cast<int>(OtaBeginNakReason::BUSY));
    EXPECT_EQ(1, static_cast<int>(OtaBeginNakReason::NO_PARTITION));
    EXPECT_EQ(2, static_cast<int>(OtaBeginNakReason::BEGIN_FAILED));
}

TEST(OtaWirePayloads, OtaEndStatusValuesMatchSpec)
{
    EXPECT_EQ(0, static_cast<int>(OtaEndStatus::OK));
    EXPECT_EQ(1, static_cast<int>(OtaEndStatus::HASH_MISMATCH));
    EXPECT_EQ(2, static_cast<int>(OtaEndStatus::WRITE_ERROR));
}

// M1 — Task 3: generateOtaPacket builds a single-packet binary frame
// with no validator-string injection.

TEST(OtaPacketBuilder, GenerateOtaBeginProducesOneFrame)
{
    auto svc = AstrOsEspNowMessageService();

    OtaBeginPayload payload{};
    payload.xferId = 0x42;
    payload.totalSize = 0x12345678;
    payload.chunkSize = 128;
    payload.totalChunks = 9600;
    for (int i = 0; i < 32; i++)
        payload.sha256Expected[i] = static_cast<uint8_t>(i);
    payload.flags = 0;

    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_BEGIN, reinterpret_cast<const uint8_t *>(&payload),
                                         sizeof(payload));
    ASSERT_EQ(1u, packets.size());
    ASSERT_EQ(20u + sizeof(payload), packets[0].size);

    uint8_t *p = packets[0].data;
    // bytes 0..15 = id (random — don't check content, just that they exist)
    EXPECT_EQ(1, p[16]); // packetNumber
    EXPECT_EQ(1, p[17]); // totalPackets
    EXPECT_EQ(static_cast<uint8_t>(AstrOsPacketType::OTA_BEGIN), p[18]);
    EXPECT_EQ(static_cast<uint8_t>(sizeof(payload)), p[19]); // payloadSize = 44

    // Payload bytes start at offset 20 with NO validator-string prefix.
    EXPECT_EQ(0x42, p[20]); // xferId byte 0 of payload
    EXPECT_EQ(0x78, p[21]); // totalSize byte 0 (LE)
    // ... (full bit-for-bit equality)
    EXPECT_EQ(0, std::memcmp(p + 20, &payload, sizeof(payload)));

    for (auto &pkt : packets)
        free(pkt.data);
}

TEST(OtaPacketBuilder, GenerateOtaPacketRejectsNonOtaType)
{
    auto svc = AstrOsEspNowMessageService();
    uint8_t dummy[1] = {0};

    auto packets = svc.generateOtaPacket(AstrOsPacketType::BASIC, dummy, sizeof(dummy));
    EXPECT_EQ(0u, packets.size()); // empty vector signals rejection
}

TEST(OtaPacketBuilder, GenerateOtaPacketRejectsOversizedPayload)
{
    auto svc = AstrOsEspNowMessageService();
    uint8_t big[ASTROS_PACKET_PAYLOAD_SIZE + 1] = {0};

    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_DATA, big, sizeof(big));
    EXPECT_EQ(0u, packets.size());
}

TEST(OtaPacketBuilder, GenerateOtaPacketAcceptsMaxPayloadSize)
{
    // Boundary test: ASTROS_PACKET_PAYLOAD_SIZE = 180 is the largest accepted
    // payload. Catches a regression to `>=` on the size check.
    auto svc = AstrOsEspNowMessageService();
    uint8_t maxPayload[ASTROS_PACKET_PAYLOAD_SIZE] = {0};

    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_DATA, maxPayload, sizeof(maxPayload));
    ASSERT_EQ(1u, packets.size());
    EXPECT_EQ(20u + ASTROS_PACKET_PAYLOAD_SIZE, packets[0].size);

    for (auto &pkt : packets)
        free(pkt.data);
}

TEST(OtaPacketBuilder, GenerateOtaDataAckProducesTinyFrame)
{
    auto svc = AstrOsEspNowMessageService();
    OtaDataAckPayload ack{};
    ack.xferId = 7;
    ack.highestContiguousSeq = 1024;
    ack.nextExpectedSeq = 1025;
    ack.windowRemaining = 8;

    auto packets =
        svc.generateOtaPacket(AstrOsPacketType::OTA_DATA_ACK, reinterpret_cast<const uint8_t *>(&ack), sizeof(ack));
    ASSERT_EQ(1u, packets.size());
    EXPECT_EQ(20u + 10u, packets[0].size); // header + 10B payload

    for (auto &pkt : packets)
        free(pkt.data);
}

// M1 — Task 4: parsePacket recognizes OTA types and skips validator-string strip.

TEST(OtaPacketParser, ParseOtaBeginReturnsBinaryPayloadIntact)
{
    auto svc = AstrOsEspNowMessageService();

    OtaBeginPayload original{};
    original.xferId = 0xAB;
    original.totalSize = 1228800;
    original.chunkSize = 128;
    original.totalChunks = 9600;
    for (int i = 0; i < 32; i++)
        original.sha256Expected[i] = static_cast<uint8_t>(0xF0 | (i & 0x0F));
    original.flags = 0;

    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_BEGIN, reinterpret_cast<const uint8_t *>(&original),
                                         sizeof(original));
    ASSERT_EQ(1u, packets.size());

    auto parsed = svc.parsePacket(packets[0].data);

    EXPECT_EQ(AstrOsPacketType::OTA_BEGIN, parsed.packetType);
    EXPECT_EQ(static_cast<int>(sizeof(OtaBeginPayload)), parsed.payloadSize);
    // payload pointer points DIRECTLY at the OtaBeginPayload bytes (no validator stripped).
    EXPECT_EQ(0, std::memcmp(parsed.payload, &original, sizeof(original)));

    for (auto &pkt : packets)
        free(pkt.data);
}

TEST(OtaPacketParser, ParseOtaDataAckReturnsBinaryPayloadIntact)
{
    auto svc = AstrOsEspNowMessageService();

    OtaDataAckPayload original{};
    original.xferId = 0x07;
    original.highestContiguousSeq = 4095;
    original.nextExpectedSeq = 4096;
    original.windowRemaining = 8;

    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_DATA_ACK, reinterpret_cast<const uint8_t *>(&original),
                                         sizeof(original));
    ASSERT_EQ(1u, packets.size());

    auto parsed = svc.parsePacket(packets[0].data);
    EXPECT_EQ(AstrOsPacketType::OTA_DATA_ACK, parsed.packetType);
    EXPECT_EQ(static_cast<int>(sizeof(OtaDataAckPayload)), parsed.payloadSize);
    EXPECT_EQ(0, std::memcmp(parsed.payload, &original, sizeof(original)));

    for (auto &pkt : packets)
        free(pkt.data);
}

TEST(OtaPacketParser, ParseRejectsOtaPacketWithOversizedPayloadByte)
{
    // Hand-construct a frame where the on-wire payloadSize byte exceeds the
    // ASTROS_PACKET_PAYLOAD_SIZE budget. parsePacket must mark this UNKNOWN
    // rather than trusting the byte and exposing downstream parsers to a
    // potential out-of-bounds read.
    uint8_t frame[20 + ASTROS_PACKET_PAYLOAD_SIZE];
    std::memset(frame, 0, sizeof(frame));
    // 16-byte id (zeros), packetNum=1, totalPackets=1
    frame[16] = 1;
    frame[17] = 1;
    frame[18] = static_cast<uint8_t>(AstrOsPacketType::OTA_BEGIN);
    frame[19] = ASTROS_PACKET_PAYLOAD_SIZE + 1; // oversize sentinel — must reject

    auto svc = AstrOsEspNowMessageService();
    auto parsed = svc.parsePacket(frame);
    EXPECT_EQ(AstrOsPacketType::UNKNOWN, parsed.packetType);
}

TEST(OtaPacketParser, ParseExistingStringTypeStillStripsValidator)
{
    // Regression: parsePacket must continue to strip the validator string
    // for non-OTA types like BASIC. (This guards against accidentally
    // routing the OTA path for everything.)
    auto svc = AstrOsEspNowMessageService();
    auto packets = svc.generatePackets(AstrOsPacketType::BASIC, "hello");
    ASSERT_EQ(1u, packets.size());

    auto parsed = svc.parsePacket(packets[0].data);
    EXPECT_EQ(AstrOsPacketType::BASIC, parsed.packetType);
    // After stripping "BASIC" + UNIT_SEPARATOR (5+1 = 6 bytes), payload is "hello" (5 bytes).
    EXPECT_EQ(5, parsed.payloadSize);
    EXPECT_EQ(0, std::memcmp(parsed.payload, "hello", 5));

    for (auto &pkt : packets)
        free(pkt.data);
}

// M1 — Task 5: POD record types + parsers in AstrOsEspNowProtocol.

TEST(OtaRecordParsers, ParseOtaBeginRoundTrip)
{
    auto svc = AstrOsEspNowMessageService();
    OtaBeginPayload original{};
    original.xferId = 0x42;
    original.totalSize = 1228800;
    original.chunkSize = 128;
    original.totalChunks = 9600;
    for (int i = 0; i < 32; i++)
        original.sha256Expected[i] = static_cast<uint8_t>(i);
    original.flags = 0;

    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_BEGIN, reinterpret_cast<const uint8_t *>(&original),
                                         sizeof(original));
    ASSERT_EQ(1u, packets.size());
    auto parsed = svc.parsePacket(packets[0].data);

    auto rec = AstrOsEspNowProtocol::parseOtaBegin(parsed);
    ASSERT_TRUE(rec.valid);
    EXPECT_EQ(0x42, rec.xferId);
    EXPECT_EQ(1228800u, rec.totalSize);
    EXPECT_EQ(128u, rec.chunkSize);
    EXPECT_EQ(9600u, rec.totalChunks);
    for (int i = 0; i < 32; i++)
        EXPECT_EQ(static_cast<uint8_t>(i), rec.sha256Expected[i]);
    EXPECT_EQ(0u, rec.flags);

    for (auto &pkt : packets)
        free(pkt.data);
}

TEST(OtaRecordParsers, ParseOtaBeginRejectsShortPayload)
{
    // Construct a malformed OTA_BEGIN with only 10 bytes of payload (not 44).
    auto svc = AstrOsEspNowMessageService();
    uint8_t shortPayload[10] = {0};
    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_BEGIN, shortPayload, sizeof(shortPayload));
    ASSERT_EQ(1u, packets.size());
    auto parsed = svc.parsePacket(packets[0].data);

    auto rec = AstrOsEspNowProtocol::parseOtaBegin(parsed);
    EXPECT_FALSE(rec.valid);

    for (auto &pkt : packets)
        free(pkt.data);
}

TEST(OtaRecordParsers, ParseOtaBeginRejectsWrongPacketType)
{
    // Cross-type confusion guard: parseOtaBegin must reject a packet whose
    // packetType is anything other than OTA_BEGIN, even if the payload size
    // happens to match.
    auto svc = AstrOsEspNowMessageService();
    OtaBeginAckPayload ack{0x42};
    auto packets =
        svc.generateOtaPacket(AstrOsPacketType::OTA_BEGIN_ACK, reinterpret_cast<const uint8_t *>(&ack), sizeof(ack));
    ASSERT_EQ(1u, packets.size());
    auto parsed = svc.parsePacket(packets[0].data);

    // parsed.packetType is OTA_BEGIN_ACK (not OTA_BEGIN). parseOtaBegin must reject.
    auto rec = AstrOsEspNowProtocol::parseOtaBegin(parsed);
    EXPECT_FALSE(rec.valid);

    for (auto &pkt : packets)
        free(pkt.data);
}

TEST(OtaRecordParsers, ParseOtaDataRoundTrip)
{
    auto svc = AstrOsEspNowMessageService();
    // OTA_DATA = 9-byte header + N bytes of firmware payload. Build one with 16 fw bytes.
    uint8_t frame[sizeof(OtaDataHeader) + 16];
    OtaDataHeader hdr{};
    hdr.xferId = 0x07;
    hdr.seq = 42;
    hdr.payloadLen = 16;
    hdr.crc16 = 0xABCD;
    std::memcpy(frame, &hdr, sizeof(hdr));
    for (int i = 0; i < 16; i++)
        frame[sizeof(hdr) + i] = static_cast<uint8_t>(0xA0 | i);

    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_DATA, frame, sizeof(frame));
    ASSERT_EQ(1u, packets.size());
    auto parsed = svc.parsePacket(packets[0].data);

    auto rec = AstrOsEspNowProtocol::parseOtaData(parsed);
    ASSERT_TRUE(rec.valid);
    EXPECT_EQ(0x07, rec.xferId);
    EXPECT_EQ(42u, rec.seq);
    EXPECT_EQ(16u, rec.payloadLen);
    EXPECT_EQ(0xABCDu, rec.crc16);
    // rec.payload points into the parsed packet's payload buffer.
    EXPECT_EQ(0xA0, rec.payload[0]);
    EXPECT_EQ(0xAF, rec.payload[15]);

    for (auto &pkt : packets)
        free(pkt.data);
}

TEST(OtaRecordParsers, ParseOtaDataRejectsPayloadLenMismatch)
{
    // header.payloadLen = 32 but the actual packet only carries 16 bytes of payload.
    auto svc = AstrOsEspNowMessageService();
    uint8_t frame[sizeof(OtaDataHeader) + 16];
    OtaDataHeader hdr{};
    hdr.xferId = 1;
    hdr.seq = 0;
    hdr.payloadLen = 32; // lying — actual payload is 16 bytes
    hdr.crc16 = 0;
    std::memcpy(frame, &hdr, sizeof(hdr));
    std::memset(frame + sizeof(hdr), 0xCC, 16);

    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_DATA, frame, sizeof(frame));
    ASSERT_EQ(1u, packets.size());
    auto parsed = svc.parsePacket(packets[0].data);

    auto rec = AstrOsEspNowProtocol::parseOtaData(parsed);
    EXPECT_FALSE(rec.valid);

    for (auto &pkt : packets)
        free(pkt.data);
}

TEST(OtaRecordParsers, ParseOtaDataRejectsZeroPayloadLen)
{
    // A header-only OTA_DATA frame (payloadLen == 0) is malformed — BulkSender
    // never emits empty chunks, and accepting one would push a zero-length
    // malloc through the dispatcher with implementation-defined behavior.
    auto svc = AstrOsEspNowMessageService();
    uint8_t frame[sizeof(OtaDataHeader)];
    OtaDataHeader hdr{};
    hdr.xferId = 1;
    hdr.seq = 0;
    hdr.payloadLen = 0;
    hdr.crc16 = 0;
    std::memcpy(frame, &hdr, sizeof(hdr));

    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_DATA, frame, sizeof(frame));
    ASSERT_EQ(1u, packets.size());
    auto parsed = svc.parsePacket(packets[0].data);

    auto rec = AstrOsEspNowProtocol::parseOtaData(parsed);
    EXPECT_FALSE(rec.valid);

    for (auto &pkt : packets)
        free(pkt.data);
}

TEST(OtaRecordParsers, ParseOtaBeginAckRoundTrip)
{
    auto svc = AstrOsEspNowMessageService();
    OtaBeginAckPayload original{0x42};
    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_BEGIN_ACK, reinterpret_cast<const uint8_t *>(&original),
                                         sizeof(original));
    ASSERT_EQ(1u, packets.size());
    auto parsed = svc.parsePacket(packets[0].data);

    auto rec = AstrOsEspNowProtocol::parseOtaBeginAck(parsed);
    ASSERT_TRUE(rec.valid);
    EXPECT_EQ(0x42, rec.xferId);

    for (auto &pkt : packets)
        free(pkt.data);
}

TEST(OtaRecordParsers, ParseOtaBeginNakRoundTrip)
{
    auto svc = AstrOsEspNowMessageService();
    OtaBeginNakPayload original{0x42, static_cast<uint8_t>(OtaBeginNakReason::NO_PARTITION)};
    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_BEGIN_NAK, reinterpret_cast<const uint8_t *>(&original),
                                         sizeof(original));
    auto parsed = svc.parsePacket(packets[0].data);

    auto rec = AstrOsEspNowProtocol::parseOtaBeginNak(parsed);
    ASSERT_TRUE(rec.valid);
    EXPECT_EQ(0x42, rec.xferId);
    EXPECT_EQ(OtaBeginNakReason::NO_PARTITION, rec.reason);

    for (auto &pkt : packets)
        free(pkt.data);
}

TEST(OtaRecordParsers, ParseOtaBeginNakRejectsOutOfRangeReason)
{
    auto svc = AstrOsEspNowMessageService();
    OtaBeginNakPayload bad{0x42, 99}; // 99 is not a valid OtaBeginNakReason
    auto packets =
        svc.generateOtaPacket(AstrOsPacketType::OTA_BEGIN_NAK, reinterpret_cast<const uint8_t *>(&bad), sizeof(bad));
    auto parsed = svc.parsePacket(packets[0].data);

    auto rec = AstrOsEspNowProtocol::parseOtaBeginNak(parsed);
    EXPECT_FALSE(rec.valid);

    for (auto &pkt : packets)
        free(pkt.data);
}

TEST(OtaRecordParsers, ParseOtaDataAckRoundTrip)
{
    auto svc = AstrOsEspNowMessageService();
    OtaDataAckPayload original{0x07, 1023, 1024, 8};
    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_DATA_ACK, reinterpret_cast<const uint8_t *>(&original),
                                         sizeof(original));
    auto parsed = svc.parsePacket(packets[0].data);

    auto rec = AstrOsEspNowProtocol::parseOtaDataAck(parsed);
    ASSERT_TRUE(rec.valid);
    EXPECT_EQ(0x07, rec.xferId);
    EXPECT_EQ(1023u, rec.highestContiguousSeq);
    EXPECT_EQ(1024u, rec.nextExpectedSeq);
    EXPECT_EQ(8, rec.windowRemaining);

    for (auto &pkt : packets)
        free(pkt.data);
}

TEST(OtaRecordParsers, ParseOtaDataNakRoundTrip)
{
    auto svc = AstrOsEspNowMessageService();
    OtaDataNakPayload original{0x07, 1023, 1024, 8, static_cast<uint8_t>(OtaDataNakReason::CRC)};
    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_DATA_NAK, reinterpret_cast<const uint8_t *>(&original),
                                         sizeof(original));
    auto parsed = svc.parsePacket(packets[0].data);

    auto rec = AstrOsEspNowProtocol::parseOtaDataNak(parsed);
    ASSERT_TRUE(rec.valid);
    EXPECT_EQ(0x07, rec.xferId);
    EXPECT_EQ(1023u, rec.highestContiguousSeq);
    EXPECT_EQ(1024u, rec.nextExpectedSeq);
    EXPECT_EQ(8, rec.windowRemaining);
    EXPECT_EQ(OtaDataNakReason::CRC, rec.reason);

    for (auto &pkt : packets)
        free(pkt.data);
}

TEST(OtaRecordParsers, ParseOtaDataNakRejectsNoneReason)
{
    // OtaDataNakReason::NONE = 0 is a C++ sentinel only — it must not appear
    // on the wire. parseOtaDataNak rejects reason byte 0 (per the comment in
    // OtaWirePayloads.hpp at the NONE declaration).
    auto svc = AstrOsEspNowMessageService();
    OtaDataNakPayload bad{0x07, 1023, 1024, 8, static_cast<uint8_t>(OtaDataNakReason::NONE)};
    auto packets =
        svc.generateOtaPacket(AstrOsPacketType::OTA_DATA_NAK, reinterpret_cast<const uint8_t *>(&bad), sizeof(bad));
    ASSERT_EQ(1u, packets.size());
    auto parsed = svc.parsePacket(packets[0].data);

    auto rec = AstrOsEspNowProtocol::parseOtaDataNak(parsed);
    EXPECT_FALSE(rec.valid);

    for (auto &pkt : packets)
        free(pkt.data);
}

TEST(OtaRecordParsers, ParseOtaDataNakRejectsAboveWriteReason)
{
    // Upper bound: reason byte > WRITE (4) is out of range. Mirrors the
    // NONE-reason rejection on the lower bound, and the analogous Begin/End
    // out-of-range tests.
    auto svc = AstrOsEspNowMessageService();
    OtaDataNakPayload bad{0x07, 1023, 1024, 8, 99};
    auto packets =
        svc.generateOtaPacket(AstrOsPacketType::OTA_DATA_NAK, reinterpret_cast<const uint8_t *>(&bad), sizeof(bad));
    ASSERT_EQ(1u, packets.size());
    auto parsed = svc.parsePacket(packets[0].data);

    auto rec = AstrOsEspNowProtocol::parseOtaDataNak(parsed);
    EXPECT_FALSE(rec.valid);

    for (auto &pkt : packets)
        free(pkt.data);
}

TEST(OtaRecordParsers, ParseOtaEndRoundTrip)
{
    auto svc = AstrOsEspNowMessageService();
    OtaEndPayload original{};
    original.xferId = 0x42;
    original.totalChunksSent = 9600;
    for (int i = 0; i < 32; i++)
        original.sha256Final[i] = static_cast<uint8_t>(0xC0 | (i & 0x0F));
    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_END, reinterpret_cast<const uint8_t *>(&original),
                                         sizeof(original));
    auto parsed = svc.parsePacket(packets[0].data);

    auto rec = AstrOsEspNowProtocol::parseOtaEnd(parsed);
    ASSERT_TRUE(rec.valid);
    EXPECT_EQ(0x42, rec.xferId);
    EXPECT_EQ(9600u, rec.totalChunksSent);
    for (int i = 0; i < 32; i++)
        EXPECT_EQ(static_cast<uint8_t>(0xC0 | (i & 0x0F)), rec.sha256Final[i]);

    for (auto &pkt : packets)
        free(pkt.data);
}

TEST(OtaRecordParsers, ParseOtaEndAckRoundTrip)
{
    auto svc = AstrOsEspNowMessageService();
    OtaEndAckPayload original{};
    original.xferId = 0x42;
    original.status = static_cast<uint8_t>(OtaEndStatus::OK);
    for (int i = 0; i < 32; i++)
        original.sha256Computed[i] = static_cast<uint8_t>(0xB0 | (i & 0x0F));
    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_END_ACK, reinterpret_cast<const uint8_t *>(&original),
                                         sizeof(original));
    auto parsed = svc.parsePacket(packets[0].data);

    auto rec = AstrOsEspNowProtocol::parseOtaEndAck(parsed);
    ASSERT_TRUE(rec.valid);
    EXPECT_EQ(0x42, rec.xferId);
    EXPECT_EQ(OtaEndStatus::OK, rec.status);
    for (int i = 0; i < 32; i++)
        EXPECT_EQ(static_cast<uint8_t>(0xB0 | (i & 0x0F)), rec.sha256Computed[i]);

    for (auto &pkt : packets)
        free(pkt.data);
}

TEST(OtaRecordParsers, ParseOtaEndAckRejectsOutOfRangeStatus)
{
    auto svc = AstrOsEspNowMessageService();
    OtaEndAckPayload bad{};
    bad.xferId = 0x42;
    bad.status = 99; // not a valid OtaEndStatus
    auto packets =
        svc.generateOtaPacket(AstrOsPacketType::OTA_END_ACK, reinterpret_cast<const uint8_t *>(&bad), sizeof(bad));
    auto parsed = svc.parsePacket(packets[0].data);

    auto rec = AstrOsEspNowProtocol::parseOtaEndAck(parsed);
    EXPECT_FALSE(rec.valid);

    for (auto &pkt : packets)
        free(pkt.data);
}

// M1 — Task 6: handlePacket dispatcher recognizes OTA types and applies role gating.

namespace
{
    astros_packet_t buildOtaPacketForDispatch(AstrOsPacketType type, const uint8_t *payload, size_t len,
                                              std::vector<astros_espnow_data_t> &keepAlive)
    {
        auto svc = AstrOsEspNowMessageService();
        auto packets = svc.generateOtaPacket(type, payload, len);
        keepAlive.insert(keepAlive.end(), packets.begin(), packets.end());
        return svc.parsePacket(packets[0].data);
    }
} // namespace

TEST(OtaDispatcher, MasterReceivesAckTypes_ReturnsUnsupportedType)
{
    PacketTracker tracker;
    std::vector<astros_espnow_data_t> keepAlive;

    OtaBeginAckPayload ack{0x42};
    auto parsed = buildOtaPacketForDispatch(AstrOsPacketType::OTA_BEGIN_ACK, reinterpret_cast<const uint8_t *>(&ack),
                                            sizeof(ack), keepAlive);

    auto result = AstrOsEspNowProtocol::handlePacket(parsed, tracker, /*isMasterNode=*/true, /*nowMs=*/0);
    EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::UnsupportedType, result.status);

    for (auto &p : keepAlive)
        free(p.data);
}

TEST(OtaDispatcher, PadawanReceivesAckType_ReturnsWrongRole)
{
    PacketTracker tracker;
    std::vector<astros_espnow_data_t> keepAlive;

    OtaBeginAckPayload ack{0x42};
    auto parsed = buildOtaPacketForDispatch(AstrOsPacketType::OTA_BEGIN_ACK, reinterpret_cast<const uint8_t *>(&ack),
                                            sizeof(ack), keepAlive);

    auto result = AstrOsEspNowProtocol::handlePacket(parsed, tracker, /*isMasterNode=*/false, /*nowMs=*/0);
    EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::WrongRole, result.status);

    for (auto &p : keepAlive)
        free(p.data);
}

TEST(OtaDispatcher, PadawanReceivesBeginType_ReturnsUnsupportedType)
{
    PacketTracker tracker;
    std::vector<astros_espnow_data_t> keepAlive;

    OtaBeginPayload begin{};
    begin.xferId = 0x42;
    auto parsed = buildOtaPacketForDispatch(AstrOsPacketType::OTA_BEGIN, reinterpret_cast<const uint8_t *>(&begin),
                                            sizeof(begin), keepAlive);

    auto result = AstrOsEspNowProtocol::handlePacket(parsed, tracker, /*isMasterNode=*/false, /*nowMs=*/0);
    EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::UnsupportedType, result.status);

    for (auto &p : keepAlive)
        free(p.data);
}

TEST(OtaDispatcher, MasterReceivesBeginType_ReturnsWrongRole)
{
    PacketTracker tracker;
    std::vector<astros_espnow_data_t> keepAlive;

    OtaBeginPayload begin{};
    begin.xferId = 0x42;
    auto parsed = buildOtaPacketForDispatch(AstrOsPacketType::OTA_BEGIN, reinterpret_cast<const uint8_t *>(&begin),
                                            sizeof(begin), keepAlive);

    auto result = AstrOsEspNowProtocol::handlePacket(parsed, tracker, /*isMasterNode=*/true, /*nowMs=*/0);
    EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::WrongRole, result.status);

    for (auto &p : keepAlive)
        free(p.data);
}

TEST(OtaDispatcher, AllUpstreamTypesGateMasterOnly)
{
    PacketTracker tracker;
    std::vector<astros_espnow_data_t> keepAlive;

    // All padawan→master types: master receives → UnsupportedType; padawan receives → WrongRole.
    AstrOsPacketType upstreamTypes[] = {
        AstrOsPacketType::OTA_BEGIN_ACK, AstrOsPacketType::OTA_BEGIN_NAK, AstrOsPacketType::OTA_DATA_ACK,
        AstrOsPacketType::OTA_DATA_NAK,  AstrOsPacketType::OTA_END_ACK,   AstrOsPacketType::OTA_FLASH_RESULT,
    };

    for (auto type : upstreamTypes)
    {
        // payload bytes don't matter for dispatch — we never parse them here.
        uint8_t dummy[sizeof(OtaFlashResultPayload)] = {0};
        size_t len = 0;
        switch (type)
        {
        case AstrOsPacketType::OTA_BEGIN_ACK:
            len = sizeof(OtaBeginAckPayload);
            break;
        case AstrOsPacketType::OTA_BEGIN_NAK:
            len = sizeof(OtaBeginNakPayload);
            break;
        case AstrOsPacketType::OTA_DATA_ACK:
            len = sizeof(OtaDataAckPayload);
            break;
        case AstrOsPacketType::OTA_DATA_NAK:
            len = sizeof(OtaDataNakPayload);
            break;
        case AstrOsPacketType::OTA_END_ACK:
            len = sizeof(OtaEndAckPayload);
            break;
        case AstrOsPacketType::OTA_FLASH_RESULT:
            len = sizeof(OtaFlashResultPayload);
            break;
        default:
            break;
        }
        auto parsed = buildOtaPacketForDispatch(type, dummy, len, keepAlive);

        auto masterResult = AstrOsEspNowProtocol::handlePacket(parsed, tracker, true, 0);
        auto padawanResult = AstrOsEspNowProtocol::handlePacket(parsed, tracker, false, 0);
        EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::UnsupportedType, masterResult.status)
            << "type=" << static_cast<int>(type);
        EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::WrongRole, padawanResult.status)
            << "type=" << static_cast<int>(type);
    }

    for (auto &p : keepAlive)
        free(p.data);
}

TEST(OtaDispatcher, AllDownstreamTypesGatePadawanOnly)
{
    PacketTracker tracker;
    std::vector<astros_espnow_data_t> keepAlive;

    // All master→padawan types: padawan receives → UnsupportedType; master receives → WrongRole.
    // For OTA_DATA we send a minimal 9-byte header + 0-byte payload (payloadLen=0 in header).
    OtaDataHeader emptyDataHdr{};
    uint8_t otaBeginBuf[sizeof(OtaBeginPayload)] = {0};
    uint8_t otaEndBuf[sizeof(OtaEndPayload)] = {0};

    struct
    {
        AstrOsPacketType type;
        const uint8_t *payload;
        size_t len;
    } downstream[] = {
        {AstrOsPacketType::OTA_BEGIN, otaBeginBuf, sizeof(otaBeginBuf)},
        {AstrOsPacketType::OTA_DATA, reinterpret_cast<const uint8_t *>(&emptyDataHdr), sizeof(emptyDataHdr)},
        {AstrOsPacketType::OTA_END, otaEndBuf, sizeof(otaEndBuf)},
    };

    for (const auto &d : downstream)
    {
        auto parsed = buildOtaPacketForDispatch(d.type, d.payload, d.len, keepAlive);
        auto padawanResult = AstrOsEspNowProtocol::handlePacket(parsed, tracker, false, 0);
        auto masterResult = AstrOsEspNowProtocol::handlePacket(parsed, tracker, true, 0);
        EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::UnsupportedType, padawanResult.status)
            << "type=" << static_cast<int>(d.type);
        EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::WrongRole, masterResult.status)
            << "type=" << static_cast<int>(d.type);
    }

    for (auto &p : keepAlive)
        free(p.data);
}

// M4 — Task 1: OtaFlashResultPayload + parseOtaFlashResult round-trip tests.
//
// All tests below use the standard packet-encode path (generateOtaPacket +
// parsePacket) so they also exercise the dispatcher and the binary frame
// builder — matching the precedent of ParseOtaEndAckRejectsOutOfRangeStatus.

TEST(OtaFlashResult, RoundTripOk)
{
    auto svc = AstrOsEspNowMessageService();
    OtaFlashResultPayload p{};
    p.xferId = 5;
    p.status = static_cast<uint8_t>(OtaFlashStatus::OK);
    p.reasonLen = 0;

    auto packets =
        svc.generateOtaPacket(AstrOsPacketType::OTA_FLASH_RESULT, reinterpret_cast<const uint8_t *>(&p), sizeof(p));
    ASSERT_EQ(1u, packets.size());
    auto parsed = svc.parsePacket(packets[0].data);

    auto r = AstrOsEspNowProtocol::parseOtaFlashResult(parsed);
    ASSERT_TRUE(r.valid);
    EXPECT_EQ(r.xferId, 5);
    EXPECT_EQ(r.status, OtaFlashStatus::OK);
    EXPECT_EQ(r.reason, "");

    for (auto &pkt : packets)
        free(pkt.data);
}

TEST(OtaFlashResult, RoundTripFlashNotImplementedWithReason)
{
    auto svc = AstrOsEspNowMessageService();
    OtaFlashResultPayload p{};
    p.xferId = 7;
    p.status = static_cast<uint8_t>(OtaFlashStatus::FLASH_NOT_IMPLEMENTED);
    const char *reason = "pr_set_1_placeholder";
    p.reasonLen = static_cast<uint8_t>(std::strlen(reason));
    std::memcpy(p.reason, reason, p.reasonLen);

    auto packets =
        svc.generateOtaPacket(AstrOsPacketType::OTA_FLASH_RESULT, reinterpret_cast<const uint8_t *>(&p), sizeof(p));
    ASSERT_EQ(1u, packets.size());
    auto parsed = svc.parsePacket(packets[0].data);

    auto r = AstrOsEspNowProtocol::parseOtaFlashResult(parsed);
    ASSERT_TRUE(r.valid);
    EXPECT_EQ(r.xferId, 7);
    EXPECT_EQ(r.status, OtaFlashStatus::FLASH_NOT_IMPLEMENTED);
    EXPECT_EQ(r.reason, "pr_set_1_placeholder");

    for (auto &pkt : packets)
        free(pkt.data);
}

TEST(OtaFlashResult, RoundTripFailedWithReason)
{
    auto svc = AstrOsEspNowMessageService();
    OtaFlashResultPayload p{};
    p.xferId = 1;
    p.status = static_cast<uint8_t>(OtaFlashStatus::FAILED);
    const char *reason = "esp_ota_set_boot_partition: ESP_ERR_INVALID_STATE";
    p.reasonLen = static_cast<uint8_t>(std::strlen(reason));
    ASSERT_LE(p.reasonLen, sizeof(p.reason));
    std::memcpy(p.reason, reason, p.reasonLen);

    auto packets =
        svc.generateOtaPacket(AstrOsPacketType::OTA_FLASH_RESULT, reinterpret_cast<const uint8_t *>(&p), sizeof(p));
    ASSERT_EQ(1u, packets.size());
    auto parsed = svc.parsePacket(packets[0].data);

    auto r = AstrOsEspNowProtocol::parseOtaFlashResult(parsed);
    ASSERT_TRUE(r.valid);
    EXPECT_EQ(r.status, OtaFlashStatus::FAILED);
    EXPECT_EQ(r.reason, "esp_ota_set_boot_partition: ESP_ERR_INVALID_STATE");

    for (auto &pkt : packets)
        free(pkt.data);
}

TEST(OtaFlashResult, RoundTripAtMaxReasonLen)
{
    // 63-byte reason: documented max. Pins the boundary against an
    // off-by-one regression (e.g., flipping `> sizeof(p.reason)` to
    // `>= sizeof(p.reason)` would silently break this case).
    auto svc = AstrOsEspNowMessageService();
    OtaFlashResultPayload p{};
    p.xferId = 9;
    p.status = static_cast<uint8_t>(OtaFlashStatus::FAILED);
    const char *reason = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    p.reasonLen = static_cast<uint8_t>(std::strlen(reason));
    ASSERT_EQ(p.reasonLen, 63);
    std::memcpy(p.reason, reason, p.reasonLen);

    auto packets =
        svc.generateOtaPacket(AstrOsPacketType::OTA_FLASH_RESULT, reinterpret_cast<const uint8_t *>(&p), sizeof(p));
    ASSERT_EQ(1u, packets.size());
    auto parsed = svc.parsePacket(packets[0].data);

    auto r = AstrOsEspNowProtocol::parseOtaFlashResult(parsed);
    ASSERT_TRUE(r.valid);
    EXPECT_EQ(r.xferId, 9);
    EXPECT_EQ(r.status, OtaFlashStatus::FAILED);
    EXPECT_EQ(r.reason.size(), 63u);
    EXPECT_EQ(r.reason, reason);

    for (auto &pkt : packets)
        free(pkt.data);
}

TEST(OtaFlashResult, RejectsTruncatedPayload)
{
    // Send a packet whose payload is one byte short of a valid OtaFlashResultPayload.
    // The parser must reject on the payloadSize check, not on anything else.
    auto svc = AstrOsEspNowMessageService();
    uint8_t shortBuf[sizeof(OtaFlashResultPayload) - 1] = {0};

    auto packets = svc.generateOtaPacket(AstrOsPacketType::OTA_FLASH_RESULT, shortBuf, sizeof(shortBuf));
    ASSERT_EQ(1u, packets.size());
    auto parsed = svc.parsePacket(packets[0].data);

    auto r = AstrOsEspNowProtocol::parseOtaFlashResult(parsed);
    EXPECT_FALSE(r.valid);

    for (auto &pkt : packets)
        free(pkt.data);
}

TEST(OtaFlashResult, RejectsUnknownStatus)
{
    auto svc = AstrOsEspNowMessageService();
    OtaFlashResultPayload p{};
    p.status = 99;

    auto packets =
        svc.generateOtaPacket(AstrOsPacketType::OTA_FLASH_RESULT, reinterpret_cast<const uint8_t *>(&p), sizeof(p));
    ASSERT_EQ(1u, packets.size());
    auto parsed = svc.parsePacket(packets[0].data);

    auto r = AstrOsEspNowProtocol::parseOtaFlashResult(parsed);
    EXPECT_FALSE(r.valid);

    for (auto &pkt : packets)
        free(pkt.data);
}

TEST(OtaFlashResult, RejectsOversizedReasonLen)
{
    auto svc = AstrOsEspNowMessageService();
    OtaFlashResultPayload p{};
    p.status = static_cast<uint8_t>(OtaFlashStatus::OK);
    p.reasonLen = static_cast<uint8_t>(sizeof(p.reason) + 1);

    auto packets =
        svc.generateOtaPacket(AstrOsPacketType::OTA_FLASH_RESULT, reinterpret_cast<const uint8_t *>(&p), sizeof(p));
    ASSERT_EQ(1u, packets.size());
    auto parsed = svc.parsePacket(packets[0].data);

    auto r = AstrOsEspNowProtocol::parseOtaFlashResult(parsed);
    EXPECT_FALSE(r.valid);

    for (auto &pkt : packets)
        free(pkt.data);
}

// ─── mapOtaFlashStatusToResult tests ─────────────────────────────────────────

TEST(MapOtaFlashStatusToResult, OkDropsReason)
{
    auto m = AstrOsEspNowProtocol::mapOtaFlashStatusToResult(OtaFlashStatus::OK, "ignored");
    EXPECT_EQ(AstrOsEspNowProtocol::PadawanStatus::OK, m.padawanStatus);
    EXPECT_TRUE(m.errorReason.empty());
}

TEST(MapOtaFlashStatusToResult, FlashNotImplementedEmptyWireReason)
{
    auto m = AstrOsEspNowProtocol::mapOtaFlashStatusToResult(OtaFlashStatus::FLASH_NOT_IMPLEMENTED, "");
    EXPECT_EQ(AstrOsEspNowProtocol::PadawanStatus::FAILED, m.padawanStatus);
    EXPECT_EQ("flash_not_implemented", m.errorReason);
}

TEST(MapOtaFlashStatusToResult, FlashNotImplementedExplicitWireReason)
{
    auto m = AstrOsEspNowProtocol::mapOtaFlashStatusToResult(OtaFlashStatus::FLASH_NOT_IMPLEMENTED, "explicit_reason");
    EXPECT_EQ(AstrOsEspNowProtocol::PadawanStatus::FAILED, m.padawanStatus);
    EXPECT_EQ("explicit_reason", m.errorReason);
}

TEST(MapOtaFlashStatusToResult, FailedEmptyWireReason)
{
    auto m = AstrOsEspNowProtocol::mapOtaFlashStatusToResult(OtaFlashStatus::FAILED, "");
    EXPECT_EQ(AstrOsEspNowProtocol::PadawanStatus::FAILED, m.padawanStatus);
    EXPECT_EQ("flash_failed", m.errorReason);
}

TEST(MapOtaFlashStatusToResult, FailedExplicitWireReason)
{
    auto m = AstrOsEspNowProtocol::mapOtaFlashStatusToResult(OtaFlashStatus::FAILED, "esp_err_xxx");
    EXPECT_EQ(AstrOsEspNowProtocol::PadawanStatus::FAILED, m.padawanStatus);
    EXPECT_EQ("esp_err_xxx", m.errorReason);
}

TEST(MapOtaFlashStatusToResult, UnknownStatusFallback)
{
    // Defense against future OtaFlashStatus enum additions that get
    // added to the wire enum + parser but forgotten in the mapper.
    auto result = AstrOsEspNowProtocol::mapOtaFlashStatusToResult(static_cast<OtaFlashStatus>(99), "");
    EXPECT_EQ(AstrOsEspNowProtocol::PadawanStatus::FAILED, result.padawanStatus);
    EXPECT_EQ("unknown_flash_status", result.errorReason);
}

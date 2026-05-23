#include <AstrOsMessaging.hpp>
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

TEST(OtaWirePayloads, OtaBeginRoundTripsViaByteArray)
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

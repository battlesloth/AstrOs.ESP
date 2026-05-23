#include <AstrOsMessaging.hpp>
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

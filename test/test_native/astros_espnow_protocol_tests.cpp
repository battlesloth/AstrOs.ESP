#include <AstrOsEspNowProtocol.hpp>
#include <AstrOsMessaging.hpp>
#include <PacketTracker.hpp>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace
{
    // Builds an astros_packet_t pointing at the caller-owned buffer `payload`.
    // packetNumber/totalPackets default to 1/1 (single-packet message).
    astros_packet_t makePacket(const char *msgId, std::string &payload, AstrOsPacketType type, int packetNumber = 1,
                               int totalPackets = 1)
    {
        astros_packet_t packet{};
        std::memset(packet.id, 0, sizeof(packet.id));
        if (msgId != nullptr)
        {
            auto len = std::min(sizeof(packet.id), std::strlen(msgId));
            std::memcpy(packet.id, msgId, len);
        }
        packet.packetNumber = packetNumber;
        packet.totalPackets = totalPackets;
        packet.packetType = type;
        packet.payloadSize = static_cast<int>(payload.size());
        packet.payload = reinterpret_cast<uint8_t *>(payload.data());
        return packet;
    }
} // namespace

// ---------------- mapResponseType ----------------

TEST(EspNowProtocol, MapResponseTypeAckNakTable)
{
    using AstrOsEspNowProtocol::mapResponseType;
    EXPECT_EQ(AstrOsInterfaceResponseType::SEND_CONFIG_ACK, mapResponseType(AstrOsPacketType::CONFIG_ACK));
    EXPECT_EQ(AstrOsInterfaceResponseType::SEND_CONFIG_NAK, mapResponseType(AstrOsPacketType::CONFIG_NAK));
    EXPECT_EQ(AstrOsInterfaceResponseType::SAVE_SCRIPT_ACK, mapResponseType(AstrOsPacketType::SCRIPT_DEPLOY_ACK));
    EXPECT_EQ(AstrOsInterfaceResponseType::SAVE_SCRIPT_NAK, mapResponseType(AstrOsPacketType::SCRIPT_DEPLOY_NAK));
    EXPECT_EQ(AstrOsInterfaceResponseType::SCRIPT_RUN_ACK, mapResponseType(AstrOsPacketType::SCRIPT_RUN_ACK));
    EXPECT_EQ(AstrOsInterfaceResponseType::SCRIPT_RUN_NAK, mapResponseType(AstrOsPacketType::SCRIPT_RUN_NAK));
    EXPECT_EQ(AstrOsInterfaceResponseType::FORMAT_SD_ACK, mapResponseType(AstrOsPacketType::FORMAT_SD_ACK));
    EXPECT_EQ(AstrOsInterfaceResponseType::FORMAT_SD_NAK, mapResponseType(AstrOsPacketType::FORMAT_SD_NAK));
}

TEST(EspNowProtocol, MapResponseTypeReturnsUnknownForNonAckTypes)
{
    using AstrOsEspNowProtocol::mapResponseType;
    EXPECT_EQ(AstrOsInterfaceResponseType::UNKNOWN, mapResponseType(AstrOsPacketType::CONFIG));
    EXPECT_EQ(AstrOsInterfaceResponseType::UNKNOWN, mapResponseType(AstrOsPacketType::SCRIPT_DEPLOY));
    EXPECT_EQ(AstrOsInterfaceResponseType::UNKNOWN, mapResponseType(AstrOsPacketType::SCRIPT_RUN));
    EXPECT_EQ(AstrOsInterfaceResponseType::UNKNOWN, mapResponseType(AstrOsPacketType::REGISTRATION_REQ));
    EXPECT_EQ(AstrOsInterfaceResponseType::UNKNOWN, mapResponseType(AstrOsPacketType::POLL));
    EXPECT_EQ(AstrOsInterfaceResponseType::UNKNOWN, mapResponseType(AstrOsPacketType::UNKNOWN));
}

// ---------------- extractPayload ----------------

TEST(EspNowProtocol, ExtractPayloadSinglePacketReturnsRawPayload)
{
    auto tracker = PacketTracker();
    std::string payload = "aa:bb:cc:dd:ee:ff";
    auto packet = makePacket("msg-id-0000000000000", payload, AstrOsPacketType::CONFIG, 1, 1);

    auto result = AstrOsEspNowProtocol::extractPayload(packet, tracker, 1000);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(payload, *result);
}

TEST(EspNowProtocol, ExtractPayloadMultiPacketFirstFragmentReturnsPending)
{
    auto tracker = PacketTracker();
    std::string frag = "first-half";
    auto packet = makePacket("multipkt000000000000", frag, AstrOsPacketType::CONFIG, 1, 2);

    auto result = AstrOsEspNowProtocol::extractPayload(packet, tracker, 1000);

    EXPECT_FALSE(result.has_value());
}

TEST(EspNowProtocol, ExtractPayloadMultiPacketCompletionReturnsAssembledPayload)
{
    auto tracker = PacketTracker();
    std::string frag1 = "first-half-";
    std::string frag2 = "second-half";

    auto p1 = makePacket("multipkt000000000000", frag1, AstrOsPacketType::CONFIG, 1, 2);
    auto p2 = makePacket("multipkt000000000000", frag2, AstrOsPacketType::CONFIG, 2, 2);

    auto r1 = AstrOsEspNowProtocol::extractPayload(p1, tracker, 1000);
    EXPECT_FALSE(r1.has_value());

    auto r2 = AstrOsEspNowProtocol::extractPayload(p2, tracker, 1200);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ("first-half-second-half", *r2);
}

TEST(EspNowProtocol, ExtractPayloadMultiPacketUsesPacketIdForTracking)
{
    auto tracker = PacketTracker();
    std::string frag = "piece-a";
    // Two packets with different ids and totalPackets=2 should not combine.
    auto pa = makePacket("id-aaaaaaaaaaaaaaaa", frag, AstrOsPacketType::CONFIG, 1, 2);
    auto pb_payload = std::string("piece-b");
    auto pb = makePacket("id-bbbbbbbbbbbbbbbb", pb_payload, AstrOsPacketType::CONFIG, 1, 2);

    auto r1 = AstrOsEspNowProtocol::extractPayload(pa, tracker, 1000);
    EXPECT_FALSE(r1.has_value());

    auto r2 = AstrOsEspNowProtocol::extractPayload(pb, tracker, 1000);
    EXPECT_FALSE(r2.has_value());
}

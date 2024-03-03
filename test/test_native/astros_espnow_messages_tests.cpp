#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <AstrOsMessaging.hpp>

using ::testing::MatchesRegex;
using ::testing::StartsWith;

TEST(EspNowMessages, InvalidPacket)
{
    std::string testString = "TEST_PACKET";

    auto values = AstrOsEspNowMessageService::generatePackets(AstrOsPacketType::REGISTRATION, AstrOsENC::REGISTRATION, testString);

    ASSERT_EQ(1, values.size())
        << [values]() -> std::string
    { for (size_t i = 0; i < values.size(); i++)
    {
        free(values[i].data);
    }; return "Incorrect number of packets generated"; }();

    memccpy(values[0].data + 20, "INVALID", 7, 7);

    auto parsed = AstrOsEspNowMessageService::parsePacket(values[0].data);

    EXPECT_EQ(AstrOsPacketType::UNKNOWN, parsed.packetType);

    for (size_t i = 0; i < values.size(); i++)
    {
        free(values[i].data);
    }
}

TEST(EspNowMessages, SinglePacket)
{
    std::string testString = "TEST_PACKET";

    auto values = AstrOsEspNowMessageService::generatePackets(AstrOsPacketType::BASIC, AstrOsENC::BASIC, testString);

    ASSERT_EQ(1, values.size())
        << [values]() -> std::string
    { for (size_t i = 0; i < values.size(); i++)
    {
        free(values[i].data);
    }; return "Incorrect number of packets generated"; }();

    auto parsed = AstrOsEspNowMessageService::parsePacket(values[0].data);

    EXPECT_EQ(1, values.size());
    EXPECT_EQ(AstrOsPacketType::BASIC, parsed.packetType);
    EXPECT_EQ(1, parsed.packetNumber);
    EXPECT_EQ(1, parsed.totalPackets);
    EXPECT_EQ(11, parsed.payloadSize);

    std::string payloadString(reinterpret_cast<char *>(parsed.payload), parsed.payloadSize);

    EXPECT_STREQ(testString.c_str(), payloadString.c_str());

    for (size_t i = 0; i < values.size(); i++)
    {
        free(values[i].data);
    }
}

TEST(EspNowMessages, MultiPacket)
{
    std::string packet1 =
        "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. Pellentesque nec nam aliquam sem et tortor consequat. No";

    std::string packet2 =
        "n consectetur a erat nam. Nec feugiat in fermentum posuere urna. Diam maecenas sed enim ut sem viverra. Aenean vel elit scelerisque mauris pellentesque pulvinar pellentesque habita";

    std::string packet3 =
        "nt morbi. Suspendisse in est ante in nibh mauris cursus. Non curabitur gravida arcu ac tortor dignissim convallis aenean. Ut ornare lectus sit amet est placerat in. Sit amet venena";

    std::string packet4 =
        "tis urna cursus eget nunc scelerisque viverra. Velit euismod in pellentesque massa placerat duis ultricies. Nisi est sit amet facilisis magna etiam tempor.";

    std::string testString = packet1 + packet2 + packet3 + packet4;

    auto values = AstrOsEspNowMessageService::generatePackets(AstrOsPacketType::BASIC, AstrOsENC::BASIC, testString);

    ASSERT_EQ(4, values.size())
        << [values]() -> std::string
    { for (size_t i = 0; i < values.size(); i++)
    {
        free(values[i].data);
    }; return "Incorrect number of packets generated"; }();

    auto parsed0 = AstrOsEspNowMessageService::parsePacket(values[0].data);

    EXPECT_EQ(AstrOsPacketType::BASIC, parsed0.packetType);
    EXPECT_EQ(1, parsed0.packetNumber);
    EXPECT_EQ(4, parsed0.totalPackets);
    EXPECT_EQ(174, parsed0.payloadSize);

    auto parsed1 = AstrOsEspNowMessageService::parsePacket(values[1].data);

    EXPECT_EQ(AstrOsPacketType::BASIC, parsed1.packetType);
    EXPECT_EQ(2, parsed1.packetNumber);
    EXPECT_EQ(4, parsed1.totalPackets);
    EXPECT_EQ(174, parsed1.payloadSize);

    auto parsed2 = AstrOsEspNowMessageService::parsePacket(values[2].data);

    EXPECT_EQ(AstrOsPacketType::BASIC, parsed2.packetType);
    EXPECT_EQ(3, parsed2.packetNumber);
    EXPECT_EQ(4, parsed2.totalPackets);
    EXPECT_EQ(174, parsed2.payloadSize);

    auto parsed3 = AstrOsEspNowMessageService::parsePacket(values[3].data);

    EXPECT_EQ(AstrOsPacketType::BASIC, parsed3.packetType);
    EXPECT_EQ(4, parsed3.packetNumber);
    EXPECT_EQ(4, parsed3.totalPackets);
    EXPECT_EQ(173, parsed3.payloadSize);

    auto payload = (uint8_t *)malloc(parsed0.payloadSize + parsed1.payloadSize + parsed2.payloadSize + parsed3.payloadSize + 1);

    size_t offset = 0;
    memcpy(payload, parsed0.payload, parsed0.payloadSize);
    offset += parsed0.payloadSize;
    memcpy(payload + offset, parsed1.payload, parsed1.payloadSize);
    offset += parsed1.payloadSize;
    memcpy(payload + offset, parsed2.payload, parsed2.payloadSize);
    offset += parsed2.payloadSize;
    memcpy(payload + offset, parsed3.payload, parsed3.payloadSize);
    offset += parsed3.payloadSize;
    payload[offset] = '\0';

    EXPECT_STREQ(testString.c_str(), (char *)payload);

    free(payload);

    for (size_t i = 0; i < values.size(); i++)
    {
        free(values[i].data);
    }
}

TEST(EspNowMessages, RegistrationRequestMessage)
{
    auto value = AstrOsEspNowMessageService::generateEspNowMsg(AstrOsPacketType::REGISTRATION_REQ)[0];
    auto parsed = AstrOsEspNowMessageService::parsePacket(value.data);

    EXPECT_EQ(AstrOsPacketType::REGISTRATION_REQ, parsed.packetType);
    EXPECT_EQ(1, parsed.packetNumber);
    EXPECT_EQ(1, parsed.totalPackets);
    EXPECT_EQ(0, parsed.payloadSize);

    free(value.data);
}

TEST(EspNowMessages, RegistrationMessage)
{
    auto value = AstrOsEspNowMessageService::generateEspNowMsg(AstrOsPacketType::REGISTRATION, "macaddress", "test")[0];
    auto parsed = AstrOsEspNowMessageService::parsePacket(value.data);

    EXPECT_EQ(AstrOsPacketType::REGISTRATION, parsed.packetType);
    EXPECT_EQ(1, parsed.packetNumber);
    EXPECT_EQ(1, parsed.totalPackets);
    EXPECT_EQ(15, parsed.payloadSize);

    std::string payloadString(reinterpret_cast<char *>(parsed.payload), parsed.payloadSize);
    std::stringstream expected;
    expected << "macaddress" << UNIT_SEPARATOR << "test";

    EXPECT_STREQ(expected.str().c_str(), payloadString.c_str());

    free(value.data);
}

TEST(EspNowMessages, RegistrationAckMessage)
{
    auto value = AstrOsEspNowMessageService::generateEspNowMsg(AstrOsPacketType::REGISTRATION_ACK, "macaddress", "test")[0];
    auto parsed = AstrOsEspNowMessageService::parsePacket(value.data);

    EXPECT_EQ(AstrOsPacketType::REGISTRATION_ACK, parsed.packetType);
    EXPECT_EQ(1, parsed.packetNumber);
    EXPECT_EQ(1, parsed.totalPackets);
    EXPECT_EQ(15, parsed.payloadSize);

    std::string payloadString(reinterpret_cast<char *>(parsed.payload), parsed.payloadSize);
    std::stringstream expected;
    expected << "macaddress" << UNIT_SEPARATOR << "test";

    EXPECT_STREQ(expected.str().c_str(), payloadString.c_str());

    free(value.data);
}

TEST(EspNowMessages, PollMessage)
{
    auto value = AstrOsEspNowMessageService::generateEspNowMsg(AstrOsPacketType::POLL, "macaddress")[0];
    auto parsed = AstrOsEspNowMessageService::parsePacket(value.data);

    EXPECT_EQ(AstrOsPacketType::POLL, parsed.packetType);
    EXPECT_EQ(1, parsed.packetNumber);
    EXPECT_EQ(1, parsed.totalPackets);
    EXPECT_EQ(10, parsed.payloadSize);

    std::string payloadString(reinterpret_cast<char *>(parsed.payload), parsed.payloadSize);
    std::stringstream expected;
    expected << "macaddress";

    EXPECT_STREQ(expected.str().c_str(), payloadString.c_str());

    free(value.data);
}

TEST(EspNowMessages, PollAckMessage)
{
    std::stringstream msg;
    msg << "name" << UNIT_SEPARATOR << "fingerprint";

    auto value = AstrOsEspNowMessageService::generateEspNowMsg(AstrOsPacketType::POLL_ACK, "macaddress", msg.str())[0];
    auto parsed = AstrOsEspNowMessageService::parsePacket(value.data);

    EXPECT_EQ(AstrOsPacketType::POLL_ACK, parsed.packetType);
    EXPECT_EQ(1, parsed.packetNumber);
    EXPECT_EQ(1, parsed.totalPackets);
    EXPECT_EQ(27, parsed.payloadSize);

    std::string payloadString(reinterpret_cast<char *>(parsed.payload), parsed.payloadSize);
    std::stringstream expected;
    expected << "macaddress" << UNIT_SEPARATOR << "name" << UNIT_SEPARATOR << "fingerprint";

    EXPECT_STREQ(expected.str().c_str(), payloadString.c_str());

    free(value.data);
}

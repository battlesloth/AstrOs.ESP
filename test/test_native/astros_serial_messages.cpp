#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <AstrOsMessaging.h>

using ::testing::MatchesRegex;
using ::testing::StartsWith;

TEST(SerialMessages, InvalidMessage)
{
    uint8_t *testData = (uint8_t *)"TEST_MESSAGE";

    auto parsed = AstrOsSerialMessageService::parseSerialMsg(testData);

    EXPECT_EQ(AstrOsSerialMessageType::UNKNOWN, parsed.messageType);

    free(parsed.message);
}

TEST(SerialMessages, HeartbeatMessage)
{
    auto value = AstrOsSerialMessageService::generateHeartBeatMsg("test");
    auto parsed = AstrOsSerialMessageService::parseSerialMsg(value.message);
    free(value.message);

    EXPECT_EQ(AstrOsSerialMessageType::HEARTBEAT, parsed.messageType);
    EXPECT_EQ(14, parsed.messageSize);

    std::string payloadString(reinterpret_cast<char *>(parsed.message), parsed.messageSize);
    std::stringstream expected;
    expected << "HEARTBEAT" << UNIT_SEPARATOR << "test";

    EXPECT_STREQ(expected.str().c_str(), payloadString.c_str());

    free(parsed.message);
}

TEST(SerialMessages, RegistrationSyncMessage)
{
    std::vector<astros_peer_data_t> peers;
    astros_peer_data_t peer1 = {"test1", "00:00:00:00:00:01"};
    astros_peer_data_t peer2 = {"test2", "00:00:00:00:00:02"};
    peers.push_back(peer1);
    peers.push_back(peer2);

    auto value = AstrOsSerialMessageService::generateRegistrationSyncMsg(peers);
    auto parsed = AstrOsSerialMessageService::parseSerialMsg(value.message);
    free(value.message);

    EXPECT_EQ(AstrOsSerialMessageType::REGISTRATION_SYNC, parsed.messageType);
    EXPECT_EQ(65, parsed.messageSize);

    std::string payloadString(reinterpret_cast<char *>(parsed.message), parsed.messageSize);
    std::stringstream expected;
    expected << "REGISTRATION_SYNC" << UNIT_SEPARATOR << "test1:00:00:00:00:00:01" << UNIT_SEPARATOR << "test2:00:00:00:00:00:02";

    EXPECT_STREQ(expected.str().c_str(), payloadString.c_str());

    free(parsed.message);
}
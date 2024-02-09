#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <AstrOsMessaging.h>
#include <AstrOsUtility.h>

using ::testing::MatchesRegex;
using ::testing::StartsWith;

TEST(SerialMessages, PollAckMessage)
{
    auto value = AstrOsSerialMessageService::generatePollAckMsg("test", "fingerprint");

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);

    auto headerParts = AstrOsStringUtils::splitString(records[0], RECORD_SEPARATOR);

    EXPECT_EQ(2, headerParts.size());
    EXPECT_EQ(static_cast<int>(AstrOsSerialMessageType::POLL_ACK), std::stoi(headerParts[0]));
    EXPECT_EQ(AstrOsSC::POLL_ACK, headerParts[1]);

    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);
    EXPECT_EQ(2, payloadParts.size());
    EXPECT_EQ("test", payloadParts[0]);
    EXPECT_EQ("fingerprint", payloadParts[1]);
}

TEST(SerialMessages, PollNakMessage)
{
    char *test = (char *)malloc(5);

    memcpy(test, "test\0", 5);

    auto value = AstrOsSerialMessageService::generatePollNakMsg(test);

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);

    auto headerParts = AstrOsStringUtils::splitString(records[0], RECORD_SEPARATOR);

    EXPECT_EQ(2, headerParts.size());
    EXPECT_EQ(static_cast<int>(AstrOsSerialMessageType::POLL_NAK), std::stoi(headerParts[0]));
    EXPECT_EQ(AstrOsSC::POLL_NAK, headerParts[1]);

    auto payloadParts = AstrOsStringUtils::splitString(records[1], RECORD_SEPARATOR);
    EXPECT_EQ(1, payloadParts.size());
    EXPECT_EQ("test", payloadParts[0]);

    free(test);
}

TEST(SerialMessages, RegistrationSyncMessage)
{
    std::vector<astros_peer_data_t> peers;
    astros_peer_data_t peer1 = {"test1", "00:00:00:00:00:01"};
    astros_peer_data_t peer2 = {"test2", "00:00:00:00:00:02"};
    peers.push_back(peer1);
    peers.push_back(peer2);

    auto value = AstrOsSerialMessageService::generateRegistrationSyncMsg(peers);

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);

    auto headerParts = AstrOsStringUtils::splitString(records[0], RECORD_SEPARATOR);

    EXPECT_EQ(2, headerParts.size());

    EXPECT_EQ(static_cast<int>(AstrOsSerialMessageType::REGISTRATION_SYNC), std::stoi(headerParts[0]));

    EXPECT_EQ(AstrOsSC::REGISTRATION_SYNC, headerParts[1]);

    auto payloadParts = AstrOsStringUtils::splitString(records[1], RECORD_SEPARATOR);

    EXPECT_EQ(2, payloadParts.size());

    auto record1 = AstrOsStringUtils::splitString(payloadParts[0], UNIT_SEPARATOR);
    auto record2 = AstrOsStringUtils::splitString(payloadParts[1], UNIT_SEPARATOR);

    EXPECT_EQ("test1", record1[0]);
    EXPECT_EQ("00:00:00:00:00:01", record1[1]);
    EXPECT_EQ("test2", record2[0]);
    EXPECT_EQ("00:00:00:00:00:02", record2[1]);
}

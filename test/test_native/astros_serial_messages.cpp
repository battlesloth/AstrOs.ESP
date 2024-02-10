#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <AstrOsMessaging.hpp>
#include <AstrOsUtility.h>

using ::testing::MatchesRegex;
using ::testing::StartsWith;

TEST(SerialMessages, PollAckMessage)
{
    auto value = AstrOsSerialMessageService::getPollAck("test", "fingerprint");

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);

    auto validation = AstrOsSerialMessageService::validateSerialMsg(value);

    ASSERT_EQ(true, validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::POLL_ACK, validation.type);
    EXPECT_STREQ("na", validation.msgId.c_str());

    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);
    ASSERT_EQ(2, payloadParts.size());
    EXPECT_EQ("test", payloadParts[0]);
    EXPECT_EQ("fingerprint", payloadParts[1]);
}

TEST(SerialMessages, PollNakMessage)
{
    char *test = (char *)malloc(5);

    memcpy(test, "test\0", 5);

    auto value = AstrOsSerialMessageService::getPollNak(test);

    auto validation = AstrOsSerialMessageService::validateSerialMsg(value);

    ASSERT_EQ(true, validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::POLL_NAK, validation.type);
    EXPECT_STREQ("na", validation.msgId.c_str());

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);

    auto payloadParts = AstrOsStringUtils::splitString(records[1], RECORD_SEPARATOR);
    ASSERT_EQ(1, payloadParts.size());
    EXPECT_EQ("test", payloadParts[0]);

    free(test);
}

TEST(SerialMessages, RegistrationSyncAckMessage)
{
    std::string msgId = "testId";

    std::vector<astros_peer_data_t> peers;
    astros_peer_data_t peer1 = {"test1", "00:00:00:00:00:01"};
    astros_peer_data_t peer2 = {"test2", "00:00:00:00:00:02"};
    peers.push_back(peer1);
    peers.push_back(peer2);

    auto value = AstrOsSerialMessageService::getRegistrationSyncAck(msgId, peers);

    auto validation = AstrOsSerialMessageService::validateSerialMsg(value);

    ASSERT_EQ(true, validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::REGISTRATION_SYNC_ACK, validation.type);
    EXPECT_STREQ(msgId.c_str(), validation.msgId.c_str());

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);

    auto payloadParts = AstrOsStringUtils::splitString(records[1], RECORD_SEPARATOR);

    ASSERT_EQ(2, payloadParts.size());

    auto record1 = AstrOsStringUtils::splitString(payloadParts[0], UNIT_SEPARATOR);
    auto record2 = AstrOsStringUtils::splitString(payloadParts[1], UNIT_SEPARATOR);

    EXPECT_EQ("test1", record1[0]);
    EXPECT_EQ("00:00:00:00:00:01", record1[1]);
    EXPECT_EQ("test2", record2[0]);
    EXPECT_EQ("00:00:00:00:00:02", record2[1]);
}

TEST(SerialMessages, DeployScriptMessage)
{
    std::string msgId = "testId";
    std::string scriptId = "scriptId";
    std::vector<std::string> controllers = {"master", "padawan1", "padawan2"};
    std::vector<std::string> scripts = {"master_script", "padawan1_script", "padawan2_script"};

    auto value = AstrOsSerialMessageService::getDeployScript(msgId, scriptId, controllers, scripts);

    auto validation = AstrOsSerialMessageService::validateSerialMsg(value);

    ASSERT_EQ(true, validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::DEPLOY_SCRIPT, validation.type);
    EXPECT_STREQ(msgId.c_str(), validation.msgId.c_str());

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);

    auto payloadParts = AstrOsStringUtils::splitString(records[1], RECORD_SEPARATOR);

    ASSERT_EQ(3, payloadParts.size());
}

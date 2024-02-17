#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <AstrOsMessaging.hpp>
#include <AstrOsUtility.h>

using ::testing::MatchesRegex;
using ::testing::StartsWith;

TEST(SerialMessages, PollAckMessage)
{
    auto value = AstrOsSerialMessageService::getPollAck("macaddress", "test", "fingerprint");

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);

    auto validation = AstrOsSerialMessageService::validateSerialMsg(value);

    ASSERT_EQ(true, validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::POLL_ACK, validation.type);
    EXPECT_STREQ("na", validation.msgId.c_str());

    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);
    ASSERT_EQ(3, payloadParts.size());
    EXPECT_EQ("macaddress", payloadParts[0]);
    EXPECT_EQ("test", payloadParts[1]);
    EXPECT_EQ("fingerprint", payloadParts[2]);
}

TEST(SerialMessages, PollNakMessage)
{
    char *test = (char *)malloc(5);

    memcpy(test, "test\0", 5);

    auto value = AstrOsSerialMessageService::getPollNak("macaddress", test);

    auto validation = AstrOsSerialMessageService::validateSerialMsg(value);

    ASSERT_EQ(true, validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::POLL_NAK, validation.type);
    EXPECT_STREQ("na", validation.msgId.c_str());

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);

    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);
    ASSERT_EQ(2, payloadParts.size());
    EXPECT_EQ("macaddress", payloadParts[0]);
    EXPECT_EQ("test", payloadParts[1]);

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

TEST(SerialMessages, DeployConfigurationMessage)
{
    std::string msgId = "testId";
    std::vector<std::string> controllers = {"master", "padawan1", "padawan2"};
    std::vector<std::string> configs = {"master_config", "padawan1_config", "padawan2_config"};

    auto value = AstrOsSerialMessageService::getDeployConfig(msgId, controllers, configs);

    auto validation = AstrOsSerialMessageService::validateSerialMsg(value);

    ASSERT_EQ(true, validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::DEPLOY_CONFIG, validation.type);
    EXPECT_STREQ(msgId.c_str(), validation.msgId.c_str());

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);

    auto payloadParts = AstrOsStringUtils::splitString(records[1], RECORD_SEPARATOR);

    ASSERT_EQ(3, payloadParts.size());
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

TEST(SerialMessages, RunScriptMessage)
{
    std::string msgId = "testId";
    std::string scriptId = "scriptId";

    auto value = AstrOsSerialMessageService::getRunScript(msgId, scriptId);

    auto validation = AstrOsSerialMessageService::validateSerialMsg(value);

    ASSERT_EQ(true, validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::RUN_SCRIPT, validation.type);
    EXPECT_STREQ(msgId.c_str(), validation.msgId.c_str());
}

TEST(SerialMessages, RunCommandMessage)
{
    std::string msgId = "testId";
    std::string controller = "controller";
    std::string command = "command";

    auto value = AstrOsSerialMessageService::getRunCommand(msgId, controller, command);

    auto validation = AstrOsSerialMessageService::validateSerialMsg(value);

    ASSERT_EQ(true, validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::RUN_COMMAND, validation.type);
    EXPECT_STREQ(msgId.c_str(), validation.msgId.c_str());
}

//=================================================================================================
// Ack/Nak messages
//=================================================================================================

void RunAckNakTest(AstrOsSerialMessageType type)
{
    std::string msgId = "testId";
    std::string controller = "controller";
    std::string data = "data";

    auto value = AstrOsSerialMessageService::getBasicAckNak(type, msgId, controller, data);

    auto validation = AstrOsSerialMessageService::validateSerialMsg(value);

    ASSERT_EQ(true, validation.valid);
    EXPECT_EQ(type, validation.type);
    EXPECT_STREQ(msgId.c_str(), validation.msgId.c_str());

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);

    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);

    ASSERT_EQ(2, payloadParts.size());
    EXPECT_EQ(controller, payloadParts[0]);
    EXPECT_EQ(data, payloadParts[1]);
}

TEST(SerialMessages, DeployConfigurationAckMessage)
{
    RunAckNakTest(AstrOsSerialMessageType::DEPLOY_CONFIG_ACK);
}

TEST(SerialMessages, DeployConfigurationNakMessage)
{
    RunAckNakTest(AstrOsSerialMessageType::DEPLOY_CONFIG_NAK);
}

TEST(SerialMessages, DeployScriptAckMessage)
{
    RunAckNakTest(AstrOsSerialMessageType::DEPLOY_SCRIPT_ACK);
}

TEST(SerialMessages, DeployScriptNakMessage)
{
    RunAckNakTest(AstrOsSerialMessageType::DEPLOY_SCRIPT_NAK);
}

TEST(SerialMessages, RunScriptAckMessage)
{
    RunAckNakTest(AstrOsSerialMessageType::RUN_SCRIPT_ACK);
}

TEST(SerialMessages, RunScriptNakMessage)
{
    RunAckNakTest(AstrOsSerialMessageType::RUN_SCRIPT_NAK);
}

TEST(SerialMessages, RunCommandAckMessage)
{
    RunAckNakTest(AstrOsSerialMessageType::RUN_COMMAND_ACK);
}

TEST(SerialMessages, RunCommandNakMessage)
{
    RunAckNakTest(AstrOsSerialMessageType::RUN_COMMAND_NAK);
}

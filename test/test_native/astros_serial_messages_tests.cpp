#include <AstrOsMessaging.hpp>
#include <AstrOsUtility.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <sstream>

// using ::testing::MatchesRegex;
// using ::testing::StartsWith;

TEST(SerialMessages, PollAckMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    // Caller supplies the firmware version explicitly so the server records the
    // version that belongs to `macaddress`, not whatever version the master
    // happens to be running when forwarding a peer's POLL_ACK.
    auto value = msgSvc.getPollAck("macaddress", "test", "fingerprint", "9.9.9");

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);

    auto validation = msgSvc.validateSerialMsg(value);

    ASSERT_EQ(true, validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::POLL_ACK, validation.type);
    EXPECT_STREQ("na", validation.msgId.c_str());

    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);
    ASSERT_EQ(4, payloadParts.size());
    EXPECT_EQ("macaddress", payloadParts[0]);
    EXPECT_EQ("test", payloadParts[1]);
    EXPECT_EQ("fingerprint", payloadParts[2]);
    EXPECT_EQ("9.9.9", payloadParts[3]);
}

TEST(SerialMessages, PollNakMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    char *test = (char *)malloc(5);

    memcpy(test, "test\0", 5);

    auto value = msgSvc.getPollNak("macaddress", test);

    auto validation = msgSvc.validateSerialMsg(value);

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
    auto msgSvc = AstrOsSerialMessageService();
    std::string msgId = "testId";

    std::vector<astros_peer_data_t> peers;
    astros_peer_data_t peer1 = {"test1", "00:00:00:00:00:01"};
    astros_peer_data_t peer2 = {"test2", "00:00:00:00:00:02"};
    peers.push_back(peer1);
    peers.push_back(peer2);

    auto value = msgSvc.getRegistrationSyncAck(msgId, peers);

    auto validation = msgSvc.validateSerialMsg(value);

    ASSERT_EQ(true, validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::REGISTRATION_SYNC_ACK, validation.type);
    EXPECT_STREQ(msgId.c_str(), validation.msgId.c_str());

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);

    auto payloadParts = AstrOsStringUtils::splitString(records[1], RECORD_SEPARATOR);

    ASSERT_EQ(2, payloadParts.size());

    auto record1 = AstrOsStringUtils::splitString(payloadParts[0], UNIT_SEPARATOR);
    auto record2 = AstrOsStringUtils::splitString(payloadParts[1], UNIT_SEPARATOR);

    EXPECT_EQ("00:00:00:00:00:01", record1[0]);
    EXPECT_EQ("test1", record1[1]);
    EXPECT_EQ("00:00:00:00:00:02", record2[0]);
    EXPECT_EQ("test2", record2[1]);
}

TEST(SerialMessages, DeployConfigurationMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    std::string msgId = "testId";
    std::vector<std::string> controllers = {"master", "padawan1", "padawan2"};
    std::vector<std::string> macs = {"mac1", "mac2", "mac3"};
    std::vector<std::string> configs = {"master_config", "padawan1_config", "padawan2_config"};

    auto value = msgSvc.getDeployConfig(msgId, macs, controllers, configs);

    auto validation = msgSvc.validateSerialMsg(value);

    ASSERT_EQ(true, validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::DEPLOY_CONFIG, validation.type);
    EXPECT_STREQ(msgId.c_str(), validation.msgId.c_str());

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);

    auto payloadParts = AstrOsStringUtils::splitString(records[1], RECORD_SEPARATOR);

    ASSERT_EQ(3, payloadParts.size());
}

TEST(SerialMessages, DeployScriptMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    std::string msgId = "testId";
    std::string scriptId = "scriptId";
    std::vector<std::string> controllers = {"master", "padawan1", "padawan2"};
    std::vector<std::string> scripts = {"master_script", "padawan1_script", "padawan2_script"};

    auto value = msgSvc.getDeployScript(msgId, scriptId, controllers, scripts);

    auto validation = msgSvc.validateSerialMsg(value);

    ASSERT_EQ(true, validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::DEPLOY_SCRIPT, validation.type);
    EXPECT_STREQ(msgId.c_str(), validation.msgId.c_str());

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);

    auto payloadParts = AstrOsStringUtils::splitString(records[1], RECORD_SEPARATOR);

    ASSERT_EQ(3, payloadParts.size());
}

TEST(SerialMessages, RunScriptMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    std::string msgId = "testId";
    std::string scriptId = "scriptId";

    auto value = msgSvc.getRunScript(msgId, scriptId);

    auto validation = msgSvc.validateSerialMsg(value);

    ASSERT_EQ(true, validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::RUN_SCRIPT, validation.type);
    EXPECT_STREQ(msgId.c_str(), validation.msgId.c_str());
}

TEST(SerialMessages, RunCommandMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    std::string msgId = "testId";
    std::string controller = "controller";
    std::string command = "command";

    auto value = msgSvc.getRunCommand(msgId, controller, command);

    auto validation = msgSvc.validateSerialMsg(value);

    ASSERT_EQ(true, validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::RUN_COMMAND, validation.type);
    EXPECT_STREQ(msgId.c_str(), validation.msgId.c_str());
}

TEST(SerialMessages, PanicStopMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    std::string msgId = "testId";

    auto value = msgSvc.getPanicStop(msgId);

    auto validation = msgSvc.validateSerialMsg(value);

    ASSERT_EQ(true, validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::PANIC_STOP, validation.type);
    EXPECT_STREQ(msgId.c_str(), validation.msgId.c_str());
}

TEST(SerialMessages, FormatSDMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    std::string msgId = "testId";

    auto value = msgSvc.getFormatSD(msgId);

    auto validation = msgSvc.validateSerialMsg(value);

    ASSERT_EQ(true, validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::FORMAT_SD, validation.type);
    EXPECT_STREQ(msgId.c_str(), validation.msgId.c_str());
}

TEST(SerialMessages, ServoTestMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    std::string msgId = "testId";
    std::string macAddress = "macAddress";
    std::string controller = "controller";
    std::string data = "data";

    auto value = msgSvc.getServoTest(msgId, macAddress, controller, data);

    auto validation = msgSvc.validateSerialMsg(value);

    ASSERT_EQ(true, validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::SERVO_TEST, validation.type);
    EXPECT_STREQ(msgId.c_str(), validation.msgId.c_str());
}

//=================================================================================================
// Ack/Nak messages
//=================================================================================================

void RunAckNakTest(AstrOsSerialMessageType type)
{
    auto msgSvc = AstrOsSerialMessageService();
    std::string msgId = "testId";
    std::string macAddress = "macAddress";
    std::string controller = "controller";
    std::string data = "data";

    auto value = msgSvc.getBasicAckNak(type, msgId, macAddress, controller, data);

    auto validation = msgSvc.validateSerialMsg(value);

    ASSERT_EQ(true, validation.valid);
    EXPECT_EQ(type, validation.type);
    EXPECT_STREQ(msgId.c_str(), validation.msgId.c_str());

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);

    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);

    ASSERT_EQ(3, payloadParts.size());
    EXPECT_EQ(macAddress, payloadParts[0]);
    EXPECT_EQ(controller, payloadParts[1]);
    EXPECT_EQ(data, payloadParts[2]);
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

TEST(SerialMessages, FormatSDAckMessage)
{
    RunAckNakTest(AstrOsSerialMessageType::FORMAT_SD_ACK);
}

TEST(SerialMessages, FormatSDNakMessage)
{
    RunAckNakTest(AstrOsSerialMessageType::FORMAT_SD_NAK);
}

TEST(SerialMessages, ServoTestAckMessage)
{
    RunAckNakTest(AstrOsSerialMessageType::SERVO_TEST_ACK);
}

//=================================================================================================
// FW_* recognition (Phase 1 wire format)
//=================================================================================================

namespace
{
    // Hand-craft a serial message that has only a header (no payload) for
    // a given FW_* type, using the same shape generateHeader produces.
    // We can't call the real generateHeader yet for FW_* types until the
    // msgTypeMap entries land in Task 1, so this helper builds the raw
    // string directly.
    std::string buildBareHeader(int typeInt, const char *validator, const std::string &msgId)
    {
        std::stringstream ss;
        ss << typeInt << RECORD_SEPARATOR << validator << RECORD_SEPARATOR << msgId << GROUP_SEPARATOR;
        return ss.str();
    }
} // namespace

TEST(SerialMessages, FwTransferBeginRecognized)
{
    auto msgSvc = AstrOsSerialMessageService();
    auto value = buildBareHeader(30, "FW_TRANSFER_BEGIN", "mid-1");

    auto validation = msgSvc.validateSerialMsg(value);

    ASSERT_TRUE(validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::FW_TRANSFER_BEGIN, validation.type);
    EXPECT_STREQ("mid-1", validation.msgId.c_str());
}

TEST(SerialMessages, FwTypesUseProtocolReservedRange)
{
    EXPECT_EQ(30, static_cast<int>(AstrOsSerialMessageType::FW_TRANSFER_BEGIN));
    EXPECT_EQ(31, static_cast<int>(AstrOsSerialMessageType::FW_TRANSFER_BEGIN_ACK));
    EXPECT_EQ(32, static_cast<int>(AstrOsSerialMessageType::FW_CHUNK));
    EXPECT_EQ(33, static_cast<int>(AstrOsSerialMessageType::FW_CHUNK_ACK));
    EXPECT_EQ(34, static_cast<int>(AstrOsSerialMessageType::FW_CHUNK_NAK));
    EXPECT_EQ(35, static_cast<int>(AstrOsSerialMessageType::FW_TRANSFER_END));
    EXPECT_EQ(36, static_cast<int>(AstrOsSerialMessageType::FW_TRANSFER_END_ACK));
    EXPECT_EQ(37, static_cast<int>(AstrOsSerialMessageType::FW_DEPLOY_BEGIN));
    EXPECT_EQ(38, static_cast<int>(AstrOsSerialMessageType::FW_PROGRESS));
    EXPECT_EQ(39, static_cast<int>(AstrOsSerialMessageType::FW_DEPLOY_DONE));
    EXPECT_EQ(40, static_cast<int>(AstrOsSerialMessageType::FW_BACKPRESSURE));
}

TEST(SerialMessages, FwTransferBeginAckOkMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    auto value = msgSvc.getFwTransferBeginAck("mid-2", "7", "OK");

    auto validation = msgSvc.validateSerialMsg(value);
    ASSERT_TRUE(validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::FW_TRANSFER_BEGIN_ACK, validation.type);
    EXPECT_STREQ("mid-2", validation.msgId.c_str());

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);
    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);
    ASSERT_EQ(2u, payloadParts.size());
    EXPECT_EQ("7", payloadParts[0]);
    EXPECT_EQ("OK", payloadParts[1]);
}

TEST(SerialMessages, FwTransferBeginAckSdFullMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    auto value = msgSvc.getFwTransferBeginAck("mid-3", "7", "sd_full");

    auto validation = msgSvc.validateSerialMsg(value);
    ASSERT_TRUE(validation.valid);

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);
    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);
    ASSERT_EQ(2u, payloadParts.size());
    EXPECT_EQ("sd_full", payloadParts[1]);
}

TEST(SerialMessages, FwChunkAckMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    auto value = msgSvc.getFwChunkAck("7", 41, 42, 14);

    auto validation = msgSvc.validateSerialMsg(value);
    ASSERT_TRUE(validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::FW_CHUNK_ACK, validation.type);
    // FW_CHUNK_ACK is a server-bound unsolicited per-chunk reply; msgId is "na"
    EXPECT_STREQ("na", validation.msgId.c_str());

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);
    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);
    ASSERT_EQ(4u, payloadParts.size());
    EXPECT_EQ("7", payloadParts[0]);
    EXPECT_EQ("41", payloadParts[1]);
    EXPECT_EQ("42", payloadParts[2]);
    EXPECT_EQ("14", payloadParts[3]);
}

TEST(SerialMessages, FwChunkNakCrcMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    auto value = msgSvc.getFwChunkNak("7", 40, "CRC");

    auto validation = msgSvc.validateSerialMsg(value);
    ASSERT_TRUE(validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::FW_CHUNK_NAK, validation.type);
    EXPECT_STREQ("na", validation.msgId.c_str());

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);
    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);
    ASSERT_EQ(3u, payloadParts.size());
    EXPECT_EQ("7", payloadParts[0]);
    EXPECT_EQ("40", payloadParts[1]);
    EXPECT_EQ("CRC", payloadParts[2]);
}

TEST(SerialMessages, FwChunkNakOutOfOrderMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    auto value = msgSvc.getFwChunkNak("7", 40, "OUT_OF_ORDER");

    auto validation = msgSvc.validateSerialMsg(value);
    ASSERT_TRUE(validation.valid);
    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);
    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);
    ASSERT_EQ(3u, payloadParts.size());
    EXPECT_EQ("OUT_OF_ORDER", payloadParts[2]);
}

TEST(SerialMessages, FwTransferEndAckOkMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    auto computedHex = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    auto value = msgSvc.getFwTransferEndAck("mid-9", "7", "OK", computedHex);

    auto validation = msgSvc.validateSerialMsg(value);
    ASSERT_TRUE(validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::FW_TRANSFER_END_ACK, validation.type);
    EXPECT_STREQ("mid-9", validation.msgId.c_str());

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);
    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);
    ASSERT_EQ(3u, payloadParts.size());
    EXPECT_EQ("7", payloadParts[0]);
    EXPECT_EQ("OK", payloadParts[1]);
    EXPECT_EQ(computedHex, payloadParts[2]);
}

TEST(SerialMessages, FwTransferEndAckHashMismatchMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    auto computedHex = "0000000000000000000000000000000000000000000000000000000000000001";
    auto value = msgSvc.getFwTransferEndAck("mid-9", "7", "HASH_MISMATCH", computedHex);

    auto validation = msgSvc.validateSerialMsg(value);
    ASSERT_TRUE(validation.valid);
    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);
    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);
    ASSERT_EQ(3u, payloadParts.size());
    EXPECT_EQ("HASH_MISMATCH", payloadParts[1]);
    EXPECT_EQ(computedHex, payloadParts[2]);
}

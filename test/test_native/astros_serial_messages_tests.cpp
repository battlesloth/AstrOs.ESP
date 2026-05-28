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
    // Caller supplies the firmware version and build variant explicitly so the
    // server records the version + variant that belong to `macaddress`, not
    // whatever the master happens to be running when forwarding a peer's
    // POLL_ACK. c.6c.1 added `variant` as the 5th field — the server's
    // controllerVariantCache is keyed by MAC → variant and drives firmware
    // asset selection (`astros-esp-<version>-<variant>-app.bin`) at OTA time.
    auto value = msgSvc.getPollAck("macaddress", "test", "fingerprint", "9.9.9", "lolin_d32_pro");

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);

    auto validation = msgSvc.validateSerialMsg(value);

    ASSERT_EQ(true, validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::POLL_ACK, validation.type);
    EXPECT_STREQ("na", validation.msgId.c_str());

    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);
    ASSERT_EQ(5, payloadParts.size());
    EXPECT_EQ("macaddress", payloadParts[0]);
    EXPECT_EQ("test", payloadParts[1]);
    EXPECT_EQ("fingerprint", payloadParts[2]);
    EXPECT_EQ("9.9.9", payloadParts[3]);
    EXPECT_EQ("lolin_d32_pro", payloadParts[4]);
}

TEST(SerialMessages, PollAckMessageWithEmptyVariant)
{
    // Legacy / Phase 1 peer (no variant reported) relays through the master
    // with an empty variant string. The wire payload must still emit 5 fields
    // — only an explicit empty 5th field is forwarded; splitString on the
    // server side strips a trailing empty so the parser sees 4 pieces, which
    // exercises its 3-OR-4-OR-5 acceptance branch and the empty-variant cache
    // guard. Pinning this so a future "drop the empty variant" optimization
    // doesn't break the legacy-peer path.
    auto msgSvc = AstrOsSerialMessageService();
    auto value = msgSvc.getPollAck("macaddress", "test", "fingerprint", "9.9.9", "");

    auto validation = msgSvc.validateSerialMsg(value);
    ASSERT_EQ(true, validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::POLL_ACK, validation.type);

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);
    // Wire-byte assertion: the payload must end with `9.9.9<US>` so the trailing
    // 5th field IS present on the wire even though splitString later strips it.
    // A regression that produced only 4 fields outright (no trailing US) would
    // break the server's variant-aware peers; assert against the raw bytes, not
    // post-split size, so the test catches that drift.
    const auto &payload = records[1];
    ASSERT_FALSE(payload.empty());
    EXPECT_EQ(UNIT_SEPARATOR, payload.back()) << "POLL_ACK must end with a trailing US delimiter to signal an explicit "
                                                 "empty variant; otherwise older parsers see a 4-field message";

    // Post-split shape (splitString strips trailing empties) — exercises the
    // server's 3-OR-4-OR-5 acceptance branch and the empty-variant cache guard.
    auto payloadParts = AstrOsStringUtils::splitString(payload, UNIT_SEPARATOR);
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
    // Constructs the raw wire-format header directly (the same shape
    // generateHeader produces) so tests can exercise validateSerialMsg
    // at the wire level without going through a builder. Used because
    // generateHeader is private on AstrOsSerialMessageService.
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
    // lastGoodSeq=40, nextExpectedSeq=41 — sender resumes from 41.
    auto value = msgSvc.getFwChunkNak("7", 40, 41, "CRC");

    auto validation = msgSvc.validateSerialMsg(value);
    ASSERT_TRUE(validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::FW_CHUNK_NAK, validation.type);
    EXPECT_STREQ("na", validation.msgId.c_str());

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);
    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);
    ASSERT_EQ(4u, payloadParts.size());
    EXPECT_EQ("7", payloadParts[0]);
    EXPECT_EQ("40", payloadParts[1]);
    EXPECT_EQ("41", payloadParts[2]);
    EXPECT_EQ("CRC", payloadParts[3]);
}

TEST(SerialMessages, FwChunkNakFirstChunkDisambiguatesViaNextExpectedSeq)
{
    auto msgSvc = AstrOsSerialMessageService();
    // First-chunk NAK: lastGoodSeq=0 is the protocol-ambiguous value, but
    // nextExpectedSeq=0 unambiguously says "send seq 0 again". A naive
    // server that computed lastGoodSeq+1 would have skipped to seq 1.
    auto value = msgSvc.getFwChunkNak("7", 0, 0, "CRC");

    auto validation = msgSvc.validateSerialMsg(value);
    ASSERT_TRUE(validation.valid);

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);
    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);
    ASSERT_EQ(4u, payloadParts.size());
    EXPECT_EQ("0", payloadParts[1]); // lastGoodSeq
    EXPECT_EQ("0", payloadParts[2]); // nextExpectedSeq — the disambiguating field
    EXPECT_EQ("CRC", payloadParts[3]);
}

TEST(SerialMessages, FwChunkNakOutOfOrderMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    auto value = msgSvc.getFwChunkNak("7", 40, 41, "OUT_OF_ORDER");

    auto validation = msgSvc.validateSerialMsg(value);
    ASSERT_TRUE(validation.valid);
    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);
    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);
    ASSERT_EQ(4u, payloadParts.size());
    EXPECT_EQ("7", payloadParts[0]);            // transferId
    EXPECT_EQ("40", payloadParts[1]);           // lastGoodSeq
    EXPECT_EQ("41", payloadParts[2]);           // nextExpectedSeq
    EXPECT_EQ("OUT_OF_ORDER", payloadParts[3]); // reasonCode
}

TEST(SerialMessages, FwChunkNakSizeMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    auto value = msgSvc.getFwChunkNak("7", 5, 6, "SIZE");

    auto validation = msgSvc.validateSerialMsg(value);
    ASSERT_TRUE(validation.valid);
    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);
    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);
    ASSERT_EQ(4u, payloadParts.size());
    EXPECT_EQ("SIZE", payloadParts[3]);
}

TEST(SerialMessages, FwChunkNakFlashFullMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    auto value = msgSvc.getFwChunkNak("7", 100, 101, "FLASH_FULL");

    auto validation = msgSvc.validateSerialMsg(value);
    ASSERT_TRUE(validation.valid);
    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);
    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);
    ASSERT_EQ(4u, payloadParts.size());
    EXPECT_EQ("FLASH_FULL", payloadParts[3]);
}

TEST(SerialMessages, FwChunkNakRejectsInvalidReasonCode)
{
    // Caller programming error: an unrecognized reason string is silently
    // unparseable on the server side. The builder returns "" to flag this
    // at the source rather than emitting an unidentifiable frame.
    auto msgSvc = AstrOsSerialMessageService();

    // Sanity foil: an otherwise-identical call with a valid reasonCode must
    // NOT return empty. Pins that emptiness is reasonCode-driven, not a
    // regression that short-circuits the whole builder.
    EXPECT_FALSE(msgSvc.getFwChunkNak("7", 0, 0, "CRC").empty());

    EXPECT_TRUE(msgSvc.getFwChunkNak("7", 0, 0, "WAT").empty());
    EXPECT_TRUE(msgSvc.getFwChunkNak("7", 0, 0, "").empty());
    EXPECT_TRUE(msgSvc.getFwChunkNak("7", 0, 0, "crc").empty());          // case-sensitive
    EXPECT_TRUE(msgSvc.getFwChunkNak("7", 0, 0, "out_of_order").empty()); // case-sensitive
    EXPECT_TRUE(msgSvc.getFwChunkNak("7", 0, 0, " CRC").empty());         // leading space
    EXPECT_TRUE(msgSvc.getFwChunkNak("7", 0, 0, "CRC ").empty());         // trailing space
    EXPECT_TRUE(msgSvc.getFwChunkNak("7", 0, 0, "OUT-OF-ORDER").empty()); // wrong separator
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

TEST(SerialMessages, FwDeployDoneAllFailedMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    std::vector<astros_fw_deploy_result_t> results;
    results.push_back({"core", "FAILED", "", "not_implemented"});
    results.push_back({"dome", "FAILED", "", "not_implemented"});

    auto value = msgSvc.getFwDeployDone("mid-d", "7", results);

    auto validation = msgSvc.validateSerialMsg(value);
    ASSERT_TRUE(validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::FW_DEPLOY_DONE, validation.type);
    EXPECT_STREQ("mid-d", validation.msgId.c_str());

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);
    auto resultRecords = AstrOsStringUtils::splitString(records[1], RECORD_SEPARATOR);
    ASSERT_EQ(2u, resultRecords.size());

    // First record starts with transfer-id, then the first result's 4 fields:
    auto firstParts = AstrOsStringUtils::splitString(resultRecords[0], UNIT_SEPARATOR);
    ASSERT_EQ(5u, firstParts.size());
    EXPECT_EQ("7", firstParts[0]);
    EXPECT_EQ("core", firstParts[1]);
    EXPECT_EQ("FAILED", firstParts[2]);
    EXPECT_EQ("", firstParts[3]);
    EXPECT_EQ("not_implemented", firstParts[4]);

    auto secondParts = AstrOsStringUtils::splitString(resultRecords[1], UNIT_SEPARATOR);
    ASSERT_EQ(4u, secondParts.size());
    EXPECT_EQ("dome", secondParts[0]);
    EXPECT_EQ("FAILED", secondParts[1]);
    EXPECT_EQ("", secondParts[2]);
    EXPECT_EQ("not_implemented", secondParts[3]);
}

//=================================================================================================
// parseFwTransferBegin (Task 7)
//=================================================================================================

TEST(SerialMessages, ParseFwTransferBeginHappyPath)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "1234567" << UNIT_SEPARATOR
            << "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" << UNIT_SEPARATOR << "4096"
            << UNIT_SEPARATOR << "core" << RECORD_SEPARATOR << "dome" << RECORD_SEPARATOR << "master";

    auto rec = parseFwTransferBegin(payload.str());
    ASSERT_TRUE(rec.valid);
    EXPECT_EQ("7", rec.transferId);
    EXPECT_EQ(1234567u, rec.totalSize);
    EXPECT_EQ("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", rec.sha256Hex);
    EXPECT_EQ(4096u, rec.chunkSize);
    ASSERT_EQ(3u, rec.targetIds.size());
    EXPECT_EQ("core", rec.targetIds[0]);
    EXPECT_EQ("dome", rec.targetIds[1]);
    EXPECT_EQ("master", rec.targetIds[2]);
}

TEST(SerialMessages, ParseFwTransferBeginTooFewFields)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "1234567"; // only 2 fields
    auto rec = parseFwTransferBegin(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwTransferBeginNonNumericSize)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "abc" << UNIT_SEPARATOR
            << "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" << UNIT_SEPARATOR << "4096"
            << UNIT_SEPARATOR << "core";
    auto rec = parseFwTransferBegin(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwTransferBeginEmptyTargetList)
{
    // Trailing empty US-field is stripped by splitString, so this payload
    // actually reaches the field-count guard, not the target-list-empty
    // guard. See ParseFwTransferBeginRsOnlyTargetField below for the test
    // that hits the target-list-empty branch.
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "100" << UNIT_SEPARATOR
            << "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" << UNIT_SEPARATOR << "4096"
            << UNIT_SEPARATOR << "";
    auto rec = parseFwTransferBegin(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwChunkHappyPath)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "42" << UNIT_SEPARATOR << "12" << UNIT_SEPARATOR << "SGVsbG8gV29ybGQh"
            << UNIT_SEPARATOR // base64 of "Hello World!" (12 bytes)
            << "abcd";

    auto rec = parseFwChunk(payload.str());
    ASSERT_TRUE(rec.valid);
    EXPECT_EQ("7", rec.transferId);
    EXPECT_EQ(42u, rec.seq);
    EXPECT_EQ(12u, rec.payloadLen);
    EXPECT_EQ("SGVsbG8gV29ybGQh", rec.base64Payload);
    EXPECT_EQ(0xabcdu, rec.crc16);
}

TEST(SerialMessages, ParseFwChunkTooFewFields)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "42" << UNIT_SEPARATOR << "12"; // 3 fields
    auto rec = parseFwChunk(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwChunkBadCrcHex)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "42" << UNIT_SEPARATOR << "12" << UNIT_SEPARATOR << "SGVsbG8gV29ybGQh"
            << UNIT_SEPARATOR << "xyz1"; // not hex
    auto rec = parseFwChunk(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwChunkCrcHexWrongLength)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "42" << UNIT_SEPARATOR << "12" << UNIT_SEPARATOR << "SGVsbG8gV29ybGQh"
            << UNIT_SEPARATOR << "abc"; // 3 chars, not 4
    auto rec = parseFwChunk(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwTransferEndHappyPath)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "9400" << UNIT_SEPARATOR
            << "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

    auto rec = parseFwTransferEnd(payload.str());
    ASSERT_TRUE(rec.valid);
    EXPECT_EQ("7", rec.transferId);
    EXPECT_EQ(9400u, rec.totalChunks);
    EXPECT_EQ("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", rec.finalSha256Hex);
}

TEST(SerialMessages, ParseFwTransferEndWrongHashLength)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "9400" << UNIT_SEPARATOR << "deadbeef"; // 8 chars, not 64
    auto rec = parseFwTransferEnd(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwTransferEndTooFewFields)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "9400";
    auto rec = parseFwTransferEnd(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwDeployBeginHappyPath)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "core" << RECORD_SEPARATOR << "dome" << RECORD_SEPARATOR << "master";

    auto rec = parseFwDeployBegin(payload.str());
    ASSERT_TRUE(rec.valid);
    EXPECT_EQ("7", rec.transferId);
    ASSERT_EQ(3u, rec.orderIds.size());
    EXPECT_EQ("core", rec.orderIds[0]);
    EXPECT_EQ("dome", rec.orderIds[1]);
    EXPECT_EQ("master", rec.orderIds[2]);
}

TEST(SerialMessages, ParseFwDeployBeginEmptyOrderList)
{
    // Trailing empty US-field is stripped by splitString, so this payload
    // actually reaches the field-count guard, not the order-list-empty
    // guard. See ParseFwDeployBeginRsOnlyOrderList below for the test
    // that hits the order-list-empty branch.
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "";
    auto rec = parseFwDeployBegin(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwDeployBeginTooFewFields)
{
    auto rec = parseFwDeployBegin("7"); // no separator, no order list
    EXPECT_FALSE(rec.valid);
}

//=================================================================================================
// FW_* parser hardening (Cleanup commit A)
//=================================================================================================

TEST(SerialMessages, ParseFwTransferBeginEmptyTransferId)
{
    std::string hash64 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    std::stringstream payload;
    payload << "" << UNIT_SEPARATOR << "100" << UNIT_SEPARATOR << hash64 << UNIT_SEPARATOR << "4096" << UNIT_SEPARATOR
            << "core";
    auto rec = parseFwTransferBegin(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwTransferBeginTotalSizeExceedsUint32)
{
    std::string hash64 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "5000000000" // > 4 GB
            << UNIT_SEPARATOR << hash64 << UNIT_SEPARATOR << "4096" << UNIT_SEPARATOR << "core";
    auto rec = parseFwTransferBegin(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwTransferBeginInvalidHexSha256)
{
    // 64 chars but contains a non-hex char ('z')
    std::string hash64 = "z3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "100" << UNIT_SEPARATOR << hash64 << UNIT_SEPARATOR << "4096" << UNIT_SEPARATOR
            << "core";
    auto rec = parseFwTransferBegin(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwChunkEmptyTransferId)
{
    std::stringstream payload;
    payload << "" << UNIT_SEPARATOR << "42" << UNIT_SEPARATOR << "12" << UNIT_SEPARATOR << "SGVsbG8gV29ybGQh"
            << UNIT_SEPARATOR << "abcd";
    auto rec = parseFwChunk(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwChunkSeqExceedsUint32)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "5000000000" << UNIT_SEPARATOR << "12" << UNIT_SEPARATOR << "SGVsbG8gV29ybGQh"
            << UNIT_SEPARATOR << "abcd";
    auto rec = parseFwChunk(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwTransferEndEmptyTransferId)
{
    std::string hash64 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    std::stringstream payload;
    payload << "" << UNIT_SEPARATOR << "9400" << UNIT_SEPARATOR << hash64;
    auto rec = parseFwTransferEnd(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwTransferEndTotalChunksExceedsUint32)
{
    std::string hash64 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "5000000000" << UNIT_SEPARATOR << hash64;
    auto rec = parseFwTransferEnd(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwTransferEndInvalidHexSha256)
{
    // 64 chars but contains a non-hex char
    std::string hash64 = "z3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "9400" << UNIT_SEPARATOR << hash64;
    auto rec = parseFwTransferEnd(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwDeployBeginEmptyTransferId)
{
    std::stringstream payload;
    payload << "" << UNIT_SEPARATOR << "core" << RECORD_SEPARATOR << "dome";
    auto rec = parseFwDeployBegin(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, FwDeployDoneEmptyResultsReturnsEmpty)
{
    auto msgSvc = AstrOsSerialMessageService();
    std::vector<astros_fw_deploy_result_t> results;
    auto value = msgSvc.getFwDeployDone("mid-empty", "7", results);
    EXPECT_TRUE(value.empty());
}

TEST(SerialMessages, FwDeployDoneSingleResultMessage)
{
    auto msgSvc = AstrOsSerialMessageService();
    std::vector<astros_fw_deploy_result_t> results;
    // FAILED record with empty finalVersion + non-empty errorOrEmpty honors the
    // cross-field convention documented in getFwDeployDone (finalVersion is empty
    // when FAILED; errorOrEmpty is non-empty when FAILED). Non-empty errorOrEmpty
    // also keeps the wire payload at 5 US-separated fields after splitString
    // (which drops trailing empty tokens).
    results.push_back({"core", "FAILED", "", "io_error"});

    auto value = msgSvc.getFwDeployDone("mid-s", "7", results);

    auto validation = msgSvc.validateSerialMsg(value);
    ASSERT_TRUE(validation.valid);

    auto records = AstrOsStringUtils::splitString(value, GROUP_SEPARATOR);
    auto resultRecords = AstrOsStringUtils::splitString(records[1], RECORD_SEPARATOR);
    ASSERT_EQ(1u, resultRecords.size()); // no RS separator emitted for single result

    auto firstParts = AstrOsStringUtils::splitString(resultRecords[0], UNIT_SEPARATOR);
    ASSERT_EQ(5u, firstParts.size());
    EXPECT_EQ("7", firstParts[0]);
    EXPECT_EQ("core", firstParts[1]);
    EXPECT_EQ("FAILED", firstParts[2]);
    EXPECT_EQ("", firstParts[3]);
    EXPECT_EQ("io_error", firstParts[4]);
}

//=================================================================================================
// FW_* empty-list rejection branches (Cleanup commit B)
//=================================================================================================

TEST(SerialMessages, ParseFwTransferBeginRsOnlyTargetField)
{
    // Reaches the targetIds-empty guard (not the field-count guard).
    // 5 US-parts with a lone RS as parts[4], which splitString(part4, RS)
    // turns into a single empty string -> rejected.
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "100" << UNIT_SEPARATOR
            << "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" << UNIT_SEPARATOR << "4096"
            << UNIT_SEPARATOR << RECORD_SEPARATOR;
    auto rec = parseFwTransferBegin(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwDeployBeginRsOnlyOrderList)
{
    // Reaches the orderIds-empty guard (not the field-count guard).
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << RECORD_SEPARATOR;
    auto rec = parseFwDeployBegin(payload.str());
    EXPECT_FALSE(rec.valid);
}

//=================================================================================================
// FW_* numeric overflow + non-numeric rejection coverage (Cleanup commit B)
//=================================================================================================

TEST(SerialMessages, ParseFwTransferBeginChunkSizeExceedsUint16)
{
    std::string hash64 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "1000000" << UNIT_SEPARATOR << hash64 << UNIT_SEPARATOR
            << "65536" // 0x10000, one over uint16 max
            << UNIT_SEPARATOR << "core";
    auto rec = parseFwTransferBegin(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwTransferBeginNonNumericChunkSize)
{
    std::string hash64 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "1000000" << UNIT_SEPARATOR << hash64 << UNIT_SEPARATOR
            << "abc" // non-numeric chunk size
            << UNIT_SEPARATOR << "core";
    auto rec = parseFwTransferBegin(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwChunkPayloadLenExceedsUint16)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "0" << UNIT_SEPARATOR << "65536" << UNIT_SEPARATOR << "AQID" << UNIT_SEPARATOR
            << "abcd";
    auto rec = parseFwChunk(payload.str());
    EXPECT_FALSE(rec.valid);
}

// payloadLen=0 would malloc(0) downstream (implementation-defined) and silently mis-route
// the resulting decoder error as a routine SIZE NAK. Reject at the parser.
TEST(SerialMessages, ParseFwChunkRejectsZeroPayloadLen)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "0" << UNIT_SEPARATOR << "0" << UNIT_SEPARATOR << "" << UNIT_SEPARATOR
            << "abcd";
    auto rec = parseFwChunk(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwTransferEndNonNumericTotalChunks)
{
    std::string hash64 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "abc" << UNIT_SEPARATOR << hash64;
    auto rec = parseFwTransferEnd(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwChunkNonNumericSeq)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "notanumber" << UNIT_SEPARATOR << "12" << UNIT_SEPARATOR << "AQID"
            << UNIT_SEPARATOR << "abcd";
    auto rec = parseFwChunk(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwChunkNonNumericPayloadLen)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "42" << UNIT_SEPARATOR << "notanumber" << UNIT_SEPARATOR << "AQID"
            << UNIT_SEPARATOR << "abcd";
    auto rec = parseFwChunk(payload.str());
    EXPECT_FALSE(rec.valid);
}

//=================================================================================================
// FW_DEPLOY_DONE contract-violation rejection (Cleanup commit C)
//=================================================================================================

TEST(SerialMessages, FwDeployDoneRejectsInvalidStatus)
{
    auto msgSvc = AstrOsSerialMessageService();
    std::vector<astros_fw_deploy_result_t> results;
    results.push_back({"core", "WAT", "1.2.0", ""}); // status must be "OK" or "FAILED"

    auto value = msgSvc.getFwDeployDone("mid-x", "7", results);
    EXPECT_TRUE(value.empty());
}

TEST(SerialMessages, FwDeployDoneRejectsInvalidStatusAmongValid)
{
    auto msgSvc = AstrOsSerialMessageService();
    std::vector<astros_fw_deploy_result_t> results;
    results.push_back({"core", "OK", "1.2.0", ""});
    results.push_back({"dome", "MAYBE", "", "huh"}); // second record is invalid

    auto value = msgSvc.getFwDeployDone("mid-x", "7", results);
    EXPECT_TRUE(value.empty()); // any invalid status fails the whole message
}

//=================================================================================================
// FW_* parser strict-unsigned + interior-empty list rejection (external PR feedback)
//=================================================================================================

TEST(SerialMessages, ParseFwTransferBeginRejectsNegativeTotalSize)
{
    // strtoul accepts "-1" and wraps it to ULONG_MAX. On 32-bit ESP targets
    // that equals UINT32_MAX and would pass `> 0xFFFFFFFFul`. The strict
    // sign-reject must catch this before strtoul runs.
    std::string hash64 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "-1" << UNIT_SEPARATOR << hash64 << UNIT_SEPARATOR << "4096" << UNIT_SEPARATOR
            << "core";
    auto rec = parseFwTransferBegin(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwTransferBeginRejectsPlusPrefixedTotalSize)
{
    std::string hash64 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "+100" << UNIT_SEPARATOR << hash64 << UNIT_SEPARATOR << "4096" << UNIT_SEPARATOR
            << "core";
    auto rec = parseFwTransferBegin(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwTransferBeginRejectsInteriorEmptyTarget)
{
    // "core<RS><RS>dome" produces targets {"core", "", "dome"}. Earlier code
    // accepted this because splitString preserves interior empty entries.
    std::string hash64 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "100" << UNIT_SEPARATOR << hash64 << UNIT_SEPARATOR << "4096" << UNIT_SEPARATOR
            << "core" << RECORD_SEPARATOR << RECORD_SEPARATOR << "dome";
    auto rec = parseFwTransferBegin(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwChunkRejectsNegativeSeq)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "-1" << UNIT_SEPARATOR << "12" << UNIT_SEPARATOR << "SGVsbG8gV29ybGQh"
            << UNIT_SEPARATOR << "abcd";
    auto rec = parseFwChunk(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwChunkRejectsNegativePayloadLen)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "0" << UNIT_SEPARATOR << "-1" << UNIT_SEPARATOR << "SGVsbG8gV29ybGQh"
            << UNIT_SEPARATOR << "abcd";
    auto rec = parseFwChunk(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwTransferEndRejectsNegativeTotalChunks)
{
    std::string hash64 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "-1" << UNIT_SEPARATOR << hash64;
    auto rec = parseFwTransferEnd(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwDeployBeginRejectsInteriorEmptyOrder)
{
    // "core<RS><RS>master" produces orderIds {"core", "", "master"}.
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "core" << RECORD_SEPARATOR << RECORD_SEPARATOR << "master";
    auto rec = parseFwDeployBegin(payload.str());
    EXPECT_FALSE(rec.valid);
}

//=================================================================================================
// FW_* parser uppercase-hex + leading-whitespace rejection (external PR feedback, second batch)
//=================================================================================================

TEST(SerialMessages, ParseFwTransferBeginRejectsUppercaseSha256)
{
    // protocol.md line 25 mandates SHA-256 is 64 *lowercase* hex chars.
    // Accepting uppercase here would let a malformed wire hash slip through
    // and later be misdiagnosed as HASH_MISMATCH when compared to the
    // master's locally computed lowercase hash.
    std::string upperHash64 = "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855";
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "100" << UNIT_SEPARATOR << upperHash64 << UNIT_SEPARATOR << "4096"
            << UNIT_SEPARATOR << "core";
    auto rec = parseFwTransferBegin(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwTransferEndRejectsUppercaseSha256)
{
    std::string upperHash64 = "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855";
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "9400" << UNIT_SEPARATOR << upperHash64;
    auto rec = parseFwTransferEnd(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwTransferBeginRejectsLeadingWhitespaceTotalSize)
{
    // strtoul skips leading whitespace; the strict digit-first check rejects it.
    std::string hash64 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << " 100" << UNIT_SEPARATOR << hash64 << UNIT_SEPARATOR << "4096" << UNIT_SEPARATOR
            << "core";
    auto rec = parseFwTransferBegin(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwChunkRejectsLeadingWhitespaceSeq)
{
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << " 42" << UNIT_SEPARATOR << "12" << UNIT_SEPARATOR << "SGVsbG8gV29ybGQh"
            << UNIT_SEPARATOR << "abcd";
    auto rec = parseFwChunk(payload.str());
    EXPECT_FALSE(rec.valid);
}

TEST(SerialMessages, ParseFwTransferEndRejectsLeadingWhitespaceTotalChunks)
{
    std::string hash64 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << " 9400" << UNIT_SEPARATOR << hash64;
    auto rec = parseFwTransferEnd(payload.str());
    EXPECT_FALSE(rec.valid);
}

//=================================================================================================
// FW_PROGRESS builder
//=================================================================================================

TEST(AstrOsSerialMessageService, FwProgress_BuildsExpectedWireFormat)
{
    auto msgSvc = AstrOsSerialMessageService();
    auto msg = msgSvc.getFwProgress("msg-1", "xfer-42", "pad1", "SENDING", 1024, 4096, "");

    auto validation = msgSvc.validateSerialMsg(msg);
    ASSERT_TRUE(validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::FW_PROGRESS, validation.type);
    EXPECT_STREQ("msg-1", validation.msgId.c_str());

    EXPECT_NE(msg.find("xfer-42"), std::string::npos);
    EXPECT_NE(msg.find("pad1"), std::string::npos);
    EXPECT_NE(msg.find("SENDING"), std::string::npos);
    EXPECT_NE(msg.find("1024"), std::string::npos);
    EXPECT_NE(msg.find("4096"), std::string::npos);

    // Payload follows GROUP_SEPARATOR; split on UNIT_SEPARATOR for field ordering.
    auto records = AstrOsStringUtils::splitString(msg, GROUP_SEPARATOR);
    ASSERT_EQ(2u, records.size());
    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);

    // 6 fields: transferId, controllerId, stage, bytesSent, totalBytes, detail.
    // AstrOsStringUtils::splitString strips trailing empty tokens, so the empty
    // detail field reduces the visible count to 5. Pin on the non-empty fields.
    ASSERT_GE(payloadParts.size(), 5u);
    EXPECT_EQ("xfer-42", payloadParts[0]);
    EXPECT_EQ("pad1", payloadParts[1]);
    EXPECT_EQ("SENDING", payloadParts[2]);
    EXPECT_EQ("1024", payloadParts[3]);
    EXPECT_EQ("4096", payloadParts[4]);
}

TEST(AstrOsSerialMessageService, FwProgress_FlashingStageRoundTrip)
{
    auto msgSvc = AstrOsSerialMessageService();
    auto msg = msgSvc.getFwProgress("msg-2", "xfer-7", "pad2", "FLASHING", 10000, 10000, "pr_set_1_placeholder");

    auto validation = msgSvc.validateSerialMsg(msg);
    ASSERT_TRUE(validation.valid);
    EXPECT_EQ(AstrOsSerialMessageType::FW_PROGRESS, validation.type);

    EXPECT_NE(msg.find("FLASHING"), std::string::npos);
    EXPECT_NE(msg.find("pr_set_1_placeholder"), std::string::npos);

    auto records = AstrOsStringUtils::splitString(msg, GROUP_SEPARATOR);
    ASSERT_EQ(2u, records.size());
    auto payloadParts = AstrOsStringUtils::splitString(records[1], UNIT_SEPARATOR);

    ASSERT_EQ(6u, payloadParts.size());
    EXPECT_EQ("xfer-7", payloadParts[0]);
    EXPECT_EQ("pad2", payloadParts[1]);
    EXPECT_EQ("FLASHING", payloadParts[2]);
    EXPECT_EQ("10000", payloadParts[3]);
    EXPECT_EQ("10000", payloadParts[4]);
    EXPECT_EQ("pr_set_1_placeholder", payloadParts[5]);
}

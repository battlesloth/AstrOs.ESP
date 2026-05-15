#include <AstrOsMessaging.hpp>
#include <AstrOsSerialProtocol.hpp>
#include <AstrOsStringUtils.hpp>
#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

namespace
{
    // decodeSerialMessage takes the already-extracted payload group
    // (RECORD_SEPARATOR-joined controller records). These helpers build
    // that payload directly — no header, no GROUP_SEPARATOR prefix.
    std::string joinUnits(std::initializer_list<std::string> parts)
    {
        std::stringstream ss;
        bool first = true;
        for (const auto &p : parts)
        {
            if (!first)
                ss << UNIT_SEPARATOR;
            ss << p;
            first = false;
        }
        return ss.str();
    }

    std::string joinRecords(std::initializer_list<std::string> entries)
    {
        std::stringstream ss;
        bool first = true;
        for (const auto &e : entries)
        {
            if (!first)
                ss << RECORD_SEPARATOR;
            ss << e;
            first = false;
        }
        return ss.str();
    }
} // namespace

// ---------------- mapResponseType ----------------

TEST(SerialProtocol, MapResponseTypeMasterTable)
{
    using namespace AstrOsSerialProtocol;
    EXPECT_EQ(AstrOsInterfaceResponseType::REGISTRATION_SYNC,
              mapResponseType(AstrOsSerialMessageType::REGISTRATION_SYNC, true));
    EXPECT_EQ(AstrOsInterfaceResponseType::SET_CONFIG, mapResponseType(AstrOsSerialMessageType::DEPLOY_CONFIG, true));
    EXPECT_EQ(AstrOsInterfaceResponseType::SAVE_SCRIPT, mapResponseType(AstrOsSerialMessageType::DEPLOY_SCRIPT, true));
    EXPECT_EQ(AstrOsInterfaceResponseType::SCRIPT_RUN, mapResponseType(AstrOsSerialMessageType::RUN_SCRIPT, true));
    EXPECT_EQ(AstrOsInterfaceResponseType::PANIC_STOP, mapResponseType(AstrOsSerialMessageType::PANIC_STOP, true));
    EXPECT_EQ(AstrOsInterfaceResponseType::FORMAT_SD, mapResponseType(AstrOsSerialMessageType::FORMAT_SD, true));
    EXPECT_EQ(AstrOsInterfaceResponseType::COMMAND, mapResponseType(AstrOsSerialMessageType::RUN_COMMAND, true));
    EXPECT_EQ(AstrOsInterfaceResponseType::SERVO_TEST, mapResponseType(AstrOsSerialMessageType::SERVO_TEST, true));
}

TEST(SerialProtocol, MapResponseTypePadawanTable)
{
    using namespace AstrOsSerialProtocol;
    EXPECT_EQ(AstrOsInterfaceResponseType::SEND_CONFIG, mapResponseType(AstrOsSerialMessageType::DEPLOY_CONFIG, false));
    EXPECT_EQ(AstrOsInterfaceResponseType::SEND_SCRIPT, mapResponseType(AstrOsSerialMessageType::DEPLOY_SCRIPT, false));
    EXPECT_EQ(AstrOsInterfaceResponseType::SEND_SCRIPT_RUN,
              mapResponseType(AstrOsSerialMessageType::RUN_SCRIPT, false));
    EXPECT_EQ(AstrOsInterfaceResponseType::SEND_PANIC_STOP,
              mapResponseType(AstrOsSerialMessageType::PANIC_STOP, false));
    EXPECT_EQ(AstrOsInterfaceResponseType::SEND_FORMAT_SD, mapResponseType(AstrOsSerialMessageType::FORMAT_SD, false));
    EXPECT_EQ(AstrOsInterfaceResponseType::SEND_COMMAND, mapResponseType(AstrOsSerialMessageType::RUN_COMMAND, false));
    EXPECT_EQ(AstrOsInterfaceResponseType::SEND_SERVO_TEST,
              mapResponseType(AstrOsSerialMessageType::SERVO_TEST, false));
}

TEST(SerialProtocol, MapResponseTypePadawanRegistrationIsUnknown)
{
    // Quirk preserved from the original handler: REGISTRATION_SYNC has no
    // padawan-side entry in the lookup table, so it falls through to
    // UNKNOWN. handleRegistrationSync itself bypasses this lookup and
    // hardcodes REGISTRATION_SYNC — decode preserves that bypass.
    EXPECT_EQ(AstrOsInterfaceResponseType::UNKNOWN,
              AstrOsSerialProtocol::mapResponseType(AstrOsSerialMessageType::REGISTRATION_SYNC, false));
}

// ---------------- REGISTRATION_SYNC ----------------

TEST(SerialProtocol, RegistrationSyncProducesBroadcastCommand)
{
    auto result = AstrOsSerialProtocol::decodeSerialMessage(AstrOsSerialMessageType::REGISTRATION_SYNC, "mid", "");

    ASSERT_EQ(1u, result.commands.size());
    EXPECT_TRUE(result.rejects.empty());

    const auto &cmd = result.commands[0];
    EXPECT_EQ(AstrOsInterfaceResponseType::REGISTRATION_SYNC, cmd.responseType);
    EXPECT_EQ("mid", cmd.msgId);
    EXPECT_EQ("", cmd.peerMac);
    EXPECT_EQ("", cmd.peerName);
    EXPECT_EQ("", cmd.message);
}

TEST(SerialProtocol, RegistrationSyncPadawanStillProducesRegistrationSync)
{
    // Matches the handleRegistrationSync quirk: the hardcoded SET bypasses
    // the padawan lookup which would otherwise return UNKNOWN.
    auto result = AstrOsSerialProtocol::decodeSerialMessage(AstrOsSerialMessageType::REGISTRATION_SYNC, "mid", "");

    ASSERT_EQ(1u, result.commands.size());
    EXPECT_EQ(AstrOsInterfaceResponseType::REGISTRATION_SYNC, result.commands[0].responseType);
}

// ---------------- DEPLOY_CONFIG ----------------

TEST(SerialProtocol, DeployConfigBroadcastMacMapsToSetConfig)
{
    const std::string payload = joinUnits({"00:00:00:00:00:00", "master", "CFG_DATA"});
    auto result = AstrOsSerialProtocol::decodeSerialMessage(AstrOsSerialMessageType::DEPLOY_CONFIG, "mid", payload);

    ASSERT_EQ(1u, result.commands.size());
    EXPECT_TRUE(result.rejects.empty());

    const auto &cmd = result.commands[0];
    EXPECT_EQ(AstrOsInterfaceResponseType::SET_CONFIG, cmd.responseType);
    EXPECT_EQ("mid", cmd.msgId);
    EXPECT_EQ("", cmd.peerMac); // broadcast mac is normalised to empty
    EXPECT_EQ("CFG_DATA", cmd.message);
}

TEST(SerialProtocol, DeployConfigSpecificMacMapsToSendConfig)
{
    const std::string payload = joinUnits({"AA:BB:CC:DD:EE:FF", "padawan", "CFG_DATA"});
    auto result = AstrOsSerialProtocol::decodeSerialMessage(AstrOsSerialMessageType::DEPLOY_CONFIG, "mid", payload);

    ASSERT_EQ(1u, result.commands.size());
    EXPECT_TRUE(result.rejects.empty());

    const auto &cmd = result.commands[0];
    EXPECT_EQ(AstrOsInterfaceResponseType::SEND_CONFIG, cmd.responseType);
    EXPECT_EQ("AA:BB:CC:DD:EE:FF", cmd.peerMac);
    EXPECT_EQ("CFG_DATA", cmd.message);
}

TEST(SerialProtocol, DeployConfigWrongPartCountRejects)
{
    // Only 2 parts instead of 3.
    const std::string payload = joinUnits({"AA:BB:CC:DD:EE:FF", "padawan"});
    auto result = AstrOsSerialProtocol::decodeSerialMessage(AstrOsSerialMessageType::DEPLOY_CONFIG, "mid", payload);

    EXPECT_TRUE(result.commands.empty());
    ASSERT_EQ(1u, result.rejects.size());
    EXPECT_EQ(AstrOsSerialProtocol::DecodeRejectReason::WRONG_PART_COUNT, result.rejects[0].reason);
}

// ---------------- DEPLOY_SCRIPT ----------------

TEST(SerialProtocol, DeployScriptReconstructsScriptPayload)
{
    // Per handler: script = msgParts[2] + UNIT_SEPARATOR + msgParts[3].
    const std::string payload = joinUnits({"00:00:00:00:00:00", "master", "scriptId", "scriptBody"});
    auto result = AstrOsSerialProtocol::decodeSerialMessage(AstrOsSerialMessageType::DEPLOY_SCRIPT, "mid", payload);

    ASSERT_EQ(1u, result.commands.size());
    EXPECT_TRUE(result.rejects.empty());

    const auto &cmd = result.commands[0];
    EXPECT_EQ(AstrOsInterfaceResponseType::SAVE_SCRIPT, cmd.responseType);

    const std::string expectedScript = std::string("scriptId") + UNIT_SEPARATOR + "scriptBody";
    EXPECT_EQ(expectedScript, cmd.message);
}

TEST(SerialProtocol, DeployScriptSpecificMacRoutesToSendScript)
{
    const std::string payload = joinUnits({"AA:BB:CC:DD:EE:FF", "padawan", "scriptId", "scriptBody"});
    auto result = AstrOsSerialProtocol::decodeSerialMessage(AstrOsSerialMessageType::DEPLOY_SCRIPT, "mid", payload);

    ASSERT_EQ(1u, result.commands.size());
    EXPECT_EQ(AstrOsInterfaceResponseType::SEND_SCRIPT, result.commands[0].responseType);
    EXPECT_EQ("AA:BB:CC:DD:EE:FF", result.commands[0].peerMac);
}

TEST(SerialProtocol, DeployScriptWrongPartCountRejects)
{
    // 3 parts instead of 4 — malformed script entry.
    const std::string payload = joinUnits({"00:00:00:00:00:00", "master", "onlyScript"});
    auto result = AstrOsSerialProtocol::decodeSerialMessage(AstrOsSerialMessageType::DEPLOY_SCRIPT, "mid", payload);

    EXPECT_TRUE(result.commands.empty());
    ASSERT_EQ(1u, result.rejects.size());
    EXPECT_EQ(AstrOsSerialProtocol::DecodeRejectReason::WRONG_PART_COUNT, result.rejects[0].reason);
}

// ---------------- Basic commands ----------------

struct BasicCommandExpectation
{
    AstrOsSerialMessageType type;
    AstrOsInterfaceResponseType masterResponse;
    AstrOsInterfaceResponseType padawanResponse;
    const char *label;
};

class BasicCommandRoutingFixture : public ::testing::TestWithParam<BasicCommandExpectation>
{
};

TEST_P(BasicCommandRoutingFixture, BroadcastRoutesToMasterResponseType)
{
    const auto &param = GetParam();
    const std::string payload = joinUnits({"00:00:00:00:00:00", "master", "VAL"});
    auto result = AstrOsSerialProtocol::decodeSerialMessage(param.type, "mid", payload);

    ASSERT_EQ(1u, result.commands.size()) << "type=" << param.label;
    EXPECT_EQ(param.masterResponse, result.commands[0].responseType) << "type=" << param.label;
    EXPECT_EQ("", result.commands[0].peerMac);
    EXPECT_EQ("VAL", result.commands[0].message);
}

TEST_P(BasicCommandRoutingFixture, SpecificMacRoutesToPadawanResponseType)
{
    const auto &param = GetParam();
    const std::string payload = joinUnits({"AA:BB:CC:DD:EE:FF", "padawan", "VAL"});
    auto result = AstrOsSerialProtocol::decodeSerialMessage(param.type, "mid", payload);

    ASSERT_EQ(1u, result.commands.size()) << "type=" << param.label;
    EXPECT_EQ(param.padawanResponse, result.commands[0].responseType) << "type=" << param.label;
    EXPECT_EQ("AA:BB:CC:DD:EE:FF", result.commands[0].peerMac);
}

INSTANTIATE_TEST_SUITE_P(
    AllBasicTypes, BasicCommandRoutingFixture,
    ::testing::Values(
        BasicCommandExpectation{AstrOsSerialMessageType::RUN_SCRIPT, AstrOsInterfaceResponseType::SCRIPT_RUN,
                                AstrOsInterfaceResponseType::SEND_SCRIPT_RUN, "RUN_SCRIPT"},
        BasicCommandExpectation{AstrOsSerialMessageType::PANIC_STOP, AstrOsInterfaceResponseType::PANIC_STOP,
                                AstrOsInterfaceResponseType::SEND_PANIC_STOP, "PANIC_STOP"},
        BasicCommandExpectation{AstrOsSerialMessageType::FORMAT_SD, AstrOsInterfaceResponseType::FORMAT_SD,
                                AstrOsInterfaceResponseType::SEND_FORMAT_SD, "FORMAT_SD"},
        BasicCommandExpectation{AstrOsSerialMessageType::RUN_COMMAND, AstrOsInterfaceResponseType::COMMAND,
                                AstrOsInterfaceResponseType::SEND_COMMAND, "RUN_COMMAND"},
        BasicCommandExpectation{AstrOsSerialMessageType::SERVO_TEST, AstrOsInterfaceResponseType::SERVO_TEST,
                                AstrOsInterfaceResponseType::SEND_SERVO_TEST, "SERVO_TEST"}));

TEST(SerialProtocol, BasicCommandEmptyDestRejects)
{
    const std::string payload = joinUnits({"", "padawan", "VAL"});
    auto result = AstrOsSerialProtocol::decodeSerialMessage(AstrOsSerialMessageType::RUN_COMMAND, "mid", payload);

    EXPECT_TRUE(result.commands.empty());
    ASSERT_EQ(1u, result.rejects.size());
    EXPECT_EQ(AstrOsSerialProtocol::DecodeRejectReason::EMPTY_DEST, result.rejects[0].reason);
}

TEST(SerialProtocol, BasicCommandEmptyValueRejects)
{
    // AstrOsStringUtils::splitString pops a single trailing empty token, so
    // a bare "mac US ctrl US" collapses to 2 parts and lands in the
    // WRONG_PART_COUNT branch. To hit the EMPTY_VALUE branch we need an
    // explicit extra trailing separator so one empty survives the pop.
    std::string entry;
    entry += "AA:BB:CC:DD:EE:FF";
    entry += UNIT_SEPARATOR;
    entry += "padawan";
    entry += UNIT_SEPARATOR;
    entry += UNIT_SEPARATOR;
    const std::string payload = entry;

    auto result = AstrOsSerialProtocol::decodeSerialMessage(AstrOsSerialMessageType::RUN_COMMAND, "mid", payload);

    EXPECT_TRUE(result.commands.empty());
    ASSERT_EQ(1u, result.rejects.size());
    EXPECT_EQ(AstrOsSerialProtocol::DecodeRejectReason::EMPTY_VALUE, result.rejects[0].reason);
}

TEST(SerialProtocol, BasicCommandWrongPartCountRejects)
{
    const std::string payload = joinUnits({"AA:BB:CC:DD:EE:FF", "padawan"});
    auto result = AstrOsSerialProtocol::decodeSerialMessage(AstrOsSerialMessageType::RUN_COMMAND, "mid", payload);

    EXPECT_TRUE(result.commands.empty());
    ASSERT_EQ(1u, result.rejects.size());
    EXPECT_EQ(AstrOsSerialProtocol::DecodeRejectReason::WRONG_PART_COUNT, result.rejects[0].reason);
}

// ---------------- Mixed valid + invalid in one message ----------------

TEST(SerialProtocol, MixedValidAndInvalidEntriesCoexist)
{
    const std::string good1 = joinUnits({"00:00:00:00:00:00", "master", "v1"});
    const std::string bad = joinUnits({"", "padawan", "v2"}); // empty dest
    const std::string good2 = joinUnits({"AA:BB:CC:DD:EE:FF", "padawan2", "v3"});
    const std::string payload = joinRecords({good1, bad, good2});

    auto result = AstrOsSerialProtocol::decodeSerialMessage(AstrOsSerialMessageType::RUN_COMMAND, "mid", payload);

    ASSERT_EQ(2u, result.commands.size());
    EXPECT_EQ(AstrOsInterfaceResponseType::COMMAND, result.commands[0].responseType);
    EXPECT_EQ("v1", result.commands[0].message);
    EXPECT_EQ(AstrOsInterfaceResponseType::SEND_COMMAND, result.commands[1].responseType);
    EXPECT_EQ("v3", result.commands[1].message);

    ASSERT_EQ(1u, result.rejects.size());
    EXPECT_EQ(AstrOsSerialProtocol::DecodeRejectReason::EMPTY_DEST, result.rejects[0].reason);
}

// ---------------- Empty payload ----------------

TEST(SerialProtocol, EmptyPayloadProducesRejectForPayloadTypes)
{
    auto result = AstrOsSerialProtocol::decodeSerialMessage(AstrOsSerialMessageType::DEPLOY_CONFIG, "mid", "");

    EXPECT_TRUE(result.commands.empty());
    ASSERT_EQ(1u, result.rejects.size());
    EXPECT_EQ(AstrOsSerialProtocol::DecodeRejectReason::EMPTY_PAYLOAD, result.rejects[0].reason);
}

TEST(SerialProtocol, EmptyPayloadDoesNotAffectRegistrationSync)
{
    auto result = AstrOsSerialProtocol::decodeSerialMessage(AstrOsSerialMessageType::REGISTRATION_SYNC, "mid", "");

    ASSERT_EQ(1u, result.commands.size());
    EXPECT_TRUE(result.rejects.empty());
    EXPECT_EQ(AstrOsInterfaceResponseType::REGISTRATION_SYNC, result.commands[0].responseType);
}

// ---------------- Unknown type ----------------

TEST(SerialProtocol, UnknownTypeProducesUnknownTypeReject)
{
    auto result = AstrOsSerialProtocol::decodeSerialMessage(AstrOsSerialMessageType::UNKNOWN, "mid", "");

    EXPECT_TRUE(result.commands.empty());
    ASSERT_EQ(1u, result.rejects.size());
    EXPECT_EQ(AstrOsSerialProtocol::DecodeRejectReason::UNKNOWN_TYPE, result.rejects[0].reason);
}

TEST(SerialProtocol, DescribeRejectReasonReturnsNonNull)
{
    using namespace AstrOsSerialProtocol;
    EXPECT_STRNE("", describeRejectReason(DecodeRejectReason::WRONG_PART_COUNT));
    EXPECT_STRNE("", describeRejectReason(DecodeRejectReason::EMPTY_DEST));
    EXPECT_STRNE("", describeRejectReason(DecodeRejectReason::EMPTY_VALUE));
    EXPECT_STRNE("", describeRejectReason(DecodeRejectReason::UNKNOWN_TYPE));
    EXPECT_STRNE("", describeRejectReason(DecodeRejectReason::EMPTY_PAYLOAD));
}

TEST(SerialProtocol, MapResponseTypeMasterFwTable)
{
    using namespace AstrOsSerialProtocol;
    EXPECT_EQ(AstrOsInterfaceResponseType::FW_TRANSFER_BEGIN,
              mapResponseType(AstrOsSerialMessageType::FW_TRANSFER_BEGIN, true));
    EXPECT_EQ(AstrOsInterfaceResponseType::FW_CHUNK, mapResponseType(AstrOsSerialMessageType::FW_CHUNK, true));
    EXPECT_EQ(AstrOsInterfaceResponseType::FW_TRANSFER_END,
              mapResponseType(AstrOsSerialMessageType::FW_TRANSFER_END, true));
    EXPECT_EQ(AstrOsInterfaceResponseType::FW_DEPLOY_BEGIN,
              mapResponseType(AstrOsSerialMessageType::FW_DEPLOY_BEGIN, true));
}

TEST(SerialProtocol, MapResponseTypePadawanFwAllUnknown)
{
    using namespace AstrOsSerialProtocol;
    // Padawans don't receive FW_* over serial; they get only ESP-NOW.
    EXPECT_EQ(AstrOsInterfaceResponseType::UNKNOWN, mapResponseType(AstrOsSerialMessageType::FW_TRANSFER_BEGIN, false));
    EXPECT_EQ(AstrOsInterfaceResponseType::UNKNOWN, mapResponseType(AstrOsSerialMessageType::FW_CHUNK, false));
    EXPECT_EQ(AstrOsInterfaceResponseType::UNKNOWN, mapResponseType(AstrOsSerialMessageType::FW_TRANSFER_END, false));
    EXPECT_EQ(AstrOsInterfaceResponseType::UNKNOWN, mapResponseType(AstrOsSerialMessageType::FW_DEPLOY_BEGIN, false));
}

// ---------------- FW_* dispatch ----------------

TEST(SerialProtocol, DecodeFwTransferBeginProducesSingleCommandWithRawPayload)
{
    using namespace AstrOsSerialProtocol;
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "1234567" << UNIT_SEPARATOR
            << "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" << UNIT_SEPARATOR << "4096"
            << UNIT_SEPARATOR << "core" << RECORD_SEPARATOR << "dome";
    const std::string raw = payload.str();

    auto result = decodeSerialMessage(AstrOsSerialMessageType::FW_TRANSFER_BEGIN, "mid-1", raw);
    ASSERT_EQ(1u, result.commands.size());
    EXPECT_TRUE(result.rejects.empty());
    const auto &cmd = result.commands[0];
    EXPECT_EQ(AstrOsInterfaceResponseType::FW_TRANSFER_BEGIN, cmd.responseType);
    EXPECT_EQ("mid-1", cmd.msgId);
    EXPECT_EQ("", cmd.peerMac);
    EXPECT_EQ("", cmd.peerName);
    EXPECT_EQ(raw, cmd.message); // raw payload passes through unchanged
}

TEST(SerialProtocol, DecodeFwChunkProducesSingleCommand)
{
    using namespace AstrOsSerialProtocol;
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "0" << UNIT_SEPARATOR << "4" << UNIT_SEPARATOR << "AQID" << UNIT_SEPARATOR
            << "abcd";
    const std::string raw = payload.str();

    auto result = decodeSerialMessage(AstrOsSerialMessageType::FW_CHUNK, "mid-c", raw);
    ASSERT_EQ(1u, result.commands.size());
    EXPECT_EQ(AstrOsInterfaceResponseType::FW_CHUNK, result.commands[0].responseType);
    EXPECT_EQ(raw, result.commands[0].message);
}

TEST(SerialProtocol, DecodeFwTransferEndProducesSingleCommand)
{
    using namespace AstrOsSerialProtocol;
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "9400" << UNIT_SEPARATOR
            << "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    const std::string raw = payload.str();

    auto result = decodeSerialMessage(AstrOsSerialMessageType::FW_TRANSFER_END, "mid-e", raw);
    ASSERT_EQ(1u, result.commands.size());
    EXPECT_EQ(AstrOsInterfaceResponseType::FW_TRANSFER_END, result.commands[0].responseType);
    EXPECT_EQ(raw, result.commands[0].message);
}

TEST(SerialProtocol, DecodeFwDeployBeginProducesSingleCommand)
{
    using namespace AstrOsSerialProtocol;
    std::stringstream payload;
    payload << "7" << UNIT_SEPARATOR << "core" << RECORD_SEPARATOR << "dome";
    const std::string raw = payload.str();

    auto result = decodeSerialMessage(AstrOsSerialMessageType::FW_DEPLOY_BEGIN, "mid-d", raw);
    ASSERT_EQ(1u, result.commands.size());
    EXPECT_EQ(AstrOsInterfaceResponseType::FW_DEPLOY_BEGIN, result.commands[0].responseType);
    EXPECT_EQ(raw, result.commands[0].message);
}

TEST(SerialProtocol, DecodeFwTransferBeginEmptyPayloadRejects)
{
    using namespace AstrOsSerialProtocol;
    auto result = decodeSerialMessage(AstrOsSerialMessageType::FW_TRANSFER_BEGIN, "mid-1", "");
    EXPECT_TRUE(result.commands.empty());
    ASSERT_EQ(1u, result.rejects.size());
    EXPECT_EQ(DecodeRejectReason::EMPTY_PAYLOAD, result.rejects[0].reason);
}

TEST(SerialProtocol, DecodeFwChunkEmptyPayloadRejects)
{
    using namespace AstrOsSerialProtocol;
    auto result = decodeSerialMessage(AstrOsSerialMessageType::FW_CHUNK, "mid-c", "");
    EXPECT_TRUE(result.commands.empty());
    ASSERT_EQ(1u, result.rejects.size());
    EXPECT_EQ(DecodeRejectReason::EMPTY_PAYLOAD, result.rejects[0].reason);
}

TEST(SerialProtocol, DecodeFwTransferEndEmptyPayloadRejects)
{
    using namespace AstrOsSerialProtocol;
    auto result = decodeSerialMessage(AstrOsSerialMessageType::FW_TRANSFER_END, "mid-e", "");
    EXPECT_TRUE(result.commands.empty());
    ASSERT_EQ(1u, result.rejects.size());
    EXPECT_EQ(DecodeRejectReason::EMPTY_PAYLOAD, result.rejects[0].reason);
}

TEST(SerialProtocol, DecodeFwDeployBeginEmptyPayloadRejects)
{
    using namespace AstrOsSerialProtocol;
    auto result = decodeSerialMessage(AstrOsSerialMessageType::FW_DEPLOY_BEGIN, "mid-d", "");
    EXPECT_TRUE(result.commands.empty());
    ASSERT_EQ(1u, result.rejects.size());
    EXPECT_EQ(DecodeRejectReason::EMPTY_PAYLOAD, result.rejects[0].reason);
}

// Contract tests — feed decodeSerialMessage the exact payload bytes produced
// by AstrOs.Server's MessageGenerator (astros_api/src/serial/message_generator.ts)
// and assert the firmware decodes each message type correctly.
//
// The tests below mirror fixtures from the server's own test suite so the two
// sides stay locked to the same wire format; if either repo changes the
// encoding, a case here fails.
//
// What the decoder sees: validateSerialMsg has already split the full wire
// message on GROUP_SEPARATOR and handed us the second half. So these helpers
// build only the payload group (RECORD_SEPARATOR-joined controller records)
// — no header, no GS prefix, no trailing newline.

#include <AstrOsMessaging.hpp>
#include <AstrOsSerialProtocol.hpp>
#include <AstrOsStringUtils.hpp>
#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

namespace
{
    std::string joinUnits(std::initializer_list<std::string> fields)
    {
        std::stringstream ss;
        bool first = true;
        for (const auto &f : fields)
        {
            if (!first)
                ss << UNIT_SEPARATOR;
            ss << f;
            first = false;
        }
        return ss.str();
    }

    // RECORD_SEPARATOR-joins the controller records that the server produces
    // between GS and EOL.
    std::string buildServerPayload(const std::vector<std::string> &records)
    {
        std::stringstream ss;
        bool first = true;
        for (const auto &r : records)
        {
            if (!first)
                ss << RECORD_SEPARATOR;
            ss << r;
            first = false;
        }
        return ss.str();
    }

    // Matches the server fixture in message_generator.test.ts:130-169.
    std::string deployConfigCfg(int maestroIdx)
    {
        std::stringstream ss;
        ss << "5@1|0|1|0;1@" << maestroIdx << ":1:9600@1:1:1:800:2000:1400:0|2:1:0:500:2500:1500:1";
        return ss.str();
    }
} // namespace

// ---------------- REGISTRATION_SYNC (type 1) ----------------

TEST(SerialProtocolServer, Server_RegistrationSync_Decodes)
{
    const std::string payload = "";

    auto result = AstrOsSerialProtocol::decodeSerialMessage(AstrOsSerialMessageType::REGISTRATION_SYNC, "123", payload);

    ASSERT_EQ(1u, result.commands.size());
    EXPECT_TRUE(result.rejects.empty());
    EXPECT_EQ(AstrOsInterfaceResponseType::REGISTRATION_SYNC, result.commands[0].responseType);
    EXPECT_EQ("123", result.commands[0].msgId);
    EXPECT_EQ("", result.commands[0].peerMac);
}

// ---------------- DEPLOY_CONFIG (type 5) ----------------

TEST(SerialProtocolServer, Server_DeployConfig_SingleBroadcast_Decodes)
{
    // Realistic config payload — internal '@', '|', ':', ';' must round-trip
    // unchanged through the decoder (they are NOT control separators).
    const std::string cfg = deployConfigCfg(4);
    const std::string payload = buildServerPayload({joinUnits({"00:00:00:00:00:00", "body", cfg})});

    auto result = AstrOsSerialProtocol::decodeSerialMessage(AstrOsSerialMessageType::DEPLOY_CONFIG, "123", payload);

    ASSERT_EQ(1u, result.commands.size());
    EXPECT_TRUE(result.rejects.empty());
    EXPECT_EQ(AstrOsInterfaceResponseType::SET_CONFIG, result.commands[0].responseType);
    EXPECT_EQ("", result.commands[0].peerMac);
    EXPECT_EQ(cfg, result.commands[0].message);
}

TEST(SerialProtocolServer, Server_DeployConfig_SpecificMac_Decodes)
{
    const std::string cfg = deployConfigCfg(9);
    const std::string payload = buildServerPayload({joinUnits({"11:11:11:11:11:11", "core", cfg})});

    auto result = AstrOsSerialProtocol::decodeSerialMessage(AstrOsSerialMessageType::DEPLOY_CONFIG, "123", payload);

    ASSERT_EQ(1u, result.commands.size());
    EXPECT_EQ(AstrOsInterfaceResponseType::SEND_CONFIG, result.commands[0].responseType);
    EXPECT_EQ("11:11:11:11:11:11", result.commands[0].peerMac);
    EXPECT_EQ(cfg, result.commands[0].message);
}

TEST(SerialProtocolServer, Server_DeployConfig_ThreeControllers_Decodes)
{
    // Mirror of message_generator.test.ts:130-169.
    const std::string cfg1 = deployConfigCfg(4);
    const std::string cfg2 = deployConfigCfg(9);
    const std::string cfg3 = deployConfigCfg(14);

    const std::string payload = buildServerPayload({
        joinUnits({"00:00:00:00:00:00", "body", cfg1}),
        joinUnits({"11:11:11:11:11:11", "core", cfg2}),
        joinUnits({"22:22:22:22:22:22", "dome", cfg3}),
    });

    auto result = AstrOsSerialProtocol::decodeSerialMessage(AstrOsSerialMessageType::DEPLOY_CONFIG, "123", payload);

    ASSERT_EQ(3u, result.commands.size());
    EXPECT_TRUE(result.rejects.empty());

    EXPECT_EQ(AstrOsInterfaceResponseType::SET_CONFIG, result.commands[0].responseType);
    EXPECT_EQ("", result.commands[0].peerMac);
    EXPECT_EQ(cfg1, result.commands[0].message);

    EXPECT_EQ(AstrOsInterfaceResponseType::SEND_CONFIG, result.commands[1].responseType);
    EXPECT_EQ("11:11:11:11:11:11", result.commands[1].peerMac);
    EXPECT_EQ(cfg2, result.commands[1].message);

    EXPECT_EQ(AstrOsInterfaceResponseType::SEND_CONFIG, result.commands[2].responseType);
    EXPECT_EQ("22:22:22:22:22:22", result.commands[2].peerMac);
    EXPECT_EQ(cfg3, result.commands[2].message);
}

// ---------------- DEPLOY_SCRIPT (type 8) ----------------

TEST(SerialProtocolServer, Server_DeployScript_SingleBroadcast_Decodes)
{
    const std::string scriptId = "script-xyz";
    const std::string body = "function loop() { digitalWrite(pin, 1); }";
    const std::string payload = buildServerPayload({joinUnits({"00:00:00:00:00:00", "body", scriptId, body})});

    auto result = AstrOsSerialProtocol::decodeSerialMessage(AstrOsSerialMessageType::DEPLOY_SCRIPT, "msg-1", payload);

    ASSERT_EQ(1u, result.commands.size());
    EXPECT_TRUE(result.rejects.empty());
    EXPECT_EQ(AstrOsInterfaceResponseType::SAVE_SCRIPT, result.commands[0].responseType);
    EXPECT_EQ("", result.commands[0].peerMac);

    const std::string expected = scriptId + UNIT_SEPARATOR + body;
    EXPECT_EQ(expected, result.commands[0].message);
}

TEST(SerialProtocolServer, Server_DeployScript_SpecificMac_Decodes)
{
    const std::string scriptId = "script-xyz";
    const std::string body = "function loop() { digitalWrite(pin, 1); }";
    const std::string payload = buildServerPayload({joinUnits({"AA:BB:CC:DD:EE:01", "dome", scriptId, body})});

    auto result = AstrOsSerialProtocol::decodeSerialMessage(AstrOsSerialMessageType::DEPLOY_SCRIPT, "msg-1", payload);

    ASSERT_EQ(1u, result.commands.size());
    EXPECT_EQ(AstrOsInterfaceResponseType::SEND_SCRIPT, result.commands[0].responseType);
    EXPECT_EQ("AA:BB:CC:DD:EE:01", result.commands[0].peerMac);
    EXPECT_EQ(scriptId + UNIT_SEPARATOR + body, result.commands[0].message);
}

TEST(SerialProtocolServer, Server_DeployScript_TwoControllers_Decodes)
{
    const std::string scriptId = "script-xyz";
    const std::string body = "script-content-here";
    const std::string payload = buildServerPayload({
        joinUnits({"AA:BB:CC:DD:EE:01", "dome", scriptId, body}),
        joinUnits({"AA:BB:CC:DD:EE:02", "body", scriptId, body}),
    });

    auto result = AstrOsSerialProtocol::decodeSerialMessage(AstrOsSerialMessageType::DEPLOY_SCRIPT, "msg-1", payload);

    ASSERT_EQ(2u, result.commands.size());
    EXPECT_TRUE(result.rejects.empty());

    EXPECT_EQ("AA:BB:CC:DD:EE:01", result.commands[0].peerMac);
    EXPECT_EQ(AstrOsInterfaceResponseType::SEND_SCRIPT, result.commands[0].responseType);
    EXPECT_EQ("AA:BB:CC:DD:EE:02", result.commands[1].peerMac);
    EXPECT_EQ(AstrOsInterfaceResponseType::SEND_SCRIPT, result.commands[1].responseType);
}

// ---------------- RUN_SCRIPT (type 11) ----------------

TEST(SerialProtocolServer, Server_RunScript_SpecificMac_Decodes)
{
    const std::string payload = buildServerPayload({joinUnits({"AA:BB:CC:DD:EE:01", "dome", "script-xyz"})});

    auto result = AstrOsSerialProtocol::decodeSerialMessage(AstrOsSerialMessageType::RUN_SCRIPT, "msg-1", payload);

    ASSERT_EQ(1u, result.commands.size());
    EXPECT_EQ(AstrOsInterfaceResponseType::SEND_SCRIPT_RUN, result.commands[0].responseType);
    EXPECT_EQ("AA:BB:CC:DD:EE:01", result.commands[0].peerMac);
    EXPECT_EQ("script-xyz", result.commands[0].message);
}

// ---------------- PANIC_STOP (type 14) ----------------

TEST(SerialProtocolServer, Server_PanicStop_LiteralPayload_Decodes)
{
    const std::string payload = buildServerPayload({joinUnits({"AA:BB:CC:DD:EE:01", "dome", "PANIC"})});

    auto result = AstrOsSerialProtocol::decodeSerialMessage(AstrOsSerialMessageType::PANIC_STOP, "msg-1", payload);

    ASSERT_EQ(1u, result.commands.size());
    EXPECT_EQ(AstrOsInterfaceResponseType::SEND_PANIC_STOP, result.commands[0].responseType);
    EXPECT_EQ("AA:BB:CC:DD:EE:01", result.commands[0].peerMac);
    EXPECT_EQ("PANIC", result.commands[0].message);
}

// ---------------- RUN_COMMAND (type 15) ----------------

TEST(SerialProtocolServer, Server_RunCommand_SingleController_Decodes)
{
    // Server never emits multi-controller RUN_COMMAND (generator :272-290).
    const std::string payload = buildServerPayload({joinUnits({"AA:BB:CC:DD:EE:01", "dome", "delay(500);"})});

    auto result = AstrOsSerialProtocol::decodeSerialMessage(AstrOsSerialMessageType::RUN_COMMAND, "msg-1", payload);

    ASSERT_EQ(1u, result.commands.size());
    EXPECT_EQ(AstrOsInterfaceResponseType::SEND_COMMAND, result.commands[0].responseType);
    EXPECT_EQ("AA:BB:CC:DD:EE:01", result.commands[0].peerMac);
    EXPECT_EQ("delay(500);", result.commands[0].message);
}

// ---------------- FORMAT_SD (type 18) ----------------

TEST(SerialProtocolServer, Server_FormatSd_LiteralPayload_Decodes)
{
    const std::string payload = buildServerPayload({joinUnits({"AA:BB:CC:DD:EE:01", "dome", "FORMAT"})});

    auto result = AstrOsSerialProtocol::decodeSerialMessage(AstrOsSerialMessageType::FORMAT_SD, "msg-1", payload);

    ASSERT_EQ(1u, result.commands.size());
    EXPECT_EQ(AstrOsInterfaceResponseType::SEND_FORMAT_SD, result.commands[0].responseType);
    EXPECT_EQ("AA:BB:CC:DD:EE:01", result.commands[0].peerMac);
    EXPECT_EQ("FORMAT", result.commands[0].message);
}

// ---------------- SERVO_TEST (type 21) ----------------

TEST(SerialProtocolServer, Server_ServoTest_SpecPreservedVerbatim_Decodes)
{
    // Server never emits multi-controller SERVO_TEST (generator :292-311).
    // The colon-delimited spec 'maestro:3:5:1500' must survive intact —
    // internal ':' is NOT a control separator.
    const std::string payload = buildServerPayload({joinUnits({"AA:BB:CC:DD:EE:01", "dome", "maestro:3:5:1500"})});

    auto result = AstrOsSerialProtocol::decodeSerialMessage(AstrOsSerialMessageType::SERVO_TEST, "msg-1", payload);

    ASSERT_EQ(1u, result.commands.size());
    EXPECT_EQ(AstrOsInterfaceResponseType::SEND_SERVO_TEST, result.commands[0].responseType);
    EXPECT_EQ("AA:BB:CC:DD:EE:01", result.commands[0].peerMac);
    EXPECT_EQ("maestro:3:5:1500", result.commands[0].message);
}

// ---------------- Parameterised multi-controller routing ----------------

struct ServerBasicExpectation
{
    AstrOsSerialMessageType type;
    AstrOsInterfaceResponseType masterResponse;
    AstrOsInterfaceResponseType padawanResponse;
    const char *value;
    const char *label;
};

class ServerBasicRoutingFixture : public ::testing::TestWithParam<ServerBasicExpectation>
{
};

TEST_P(ServerBasicRoutingFixture, TwoControllerRoundTrip)
{
    const auto &p = GetParam();
    const std::string payload = buildServerPayload({
        joinUnits({"00:00:00:00:00:00", "master", p.value}),
        joinUnits({"AA:BB:CC:DD:EE:01", "dome", p.value}),
    });

    auto result = AstrOsSerialProtocol::decodeSerialMessage(p.type, "msg-1", payload);

    ASSERT_EQ(2u, result.commands.size()) << "type=" << p.label;
    EXPECT_TRUE(result.rejects.empty()) << "type=" << p.label;

    EXPECT_EQ(p.masterResponse, result.commands[0].responseType) << "type=" << p.label;
    EXPECT_EQ("", result.commands[0].peerMac);
    EXPECT_EQ(p.value, result.commands[0].message);

    EXPECT_EQ(p.padawanResponse, result.commands[1].responseType) << "type=" << p.label;
    EXPECT_EQ("AA:BB:CC:DD:EE:01", result.commands[1].peerMac);
    EXPECT_EQ(p.value, result.commands[1].message);
}

// RUN_COMMAND and SERVO_TEST are intentionally omitted — the server never
// produces multi-controller variants for those types, so a two-controller
// fixture would not reflect real traffic.
INSTANTIATE_TEST_SUITE_P(
    ServerMultiController, ServerBasicRoutingFixture,
    ::testing::Values(ServerBasicExpectation{AstrOsSerialMessageType::RUN_SCRIPT,
                                             AstrOsInterfaceResponseType::SCRIPT_RUN,
                                             AstrOsInterfaceResponseType::SEND_SCRIPT_RUN, "script-xyz", "RUN_SCRIPT"},
                      ServerBasicExpectation{AstrOsSerialMessageType::PANIC_STOP,
                                             AstrOsInterfaceResponseType::PANIC_STOP,
                                             AstrOsInterfaceResponseType::SEND_PANIC_STOP, "PANIC", "PANIC_STOP"},
                      ServerBasicExpectation{AstrOsSerialMessageType::FORMAT_SD, AstrOsInterfaceResponseType::FORMAT_SD,
                                             AstrOsInterfaceResponseType::SEND_FORMAT_SD, "FORMAT", "FORMAT_SD"}));

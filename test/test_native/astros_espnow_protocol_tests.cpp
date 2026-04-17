#include <AstrOsEspNowProtocol.hpp>
#include <AstrOsMessaging.hpp>
#include <AstrOsStringUtils.hpp>
#include <PacketTracker.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <sstream>
#include <string>

namespace
{
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
    EXPECT_EQ(AstrOsInterfaceResponseType::COMMAND_ACK, mapResponseType(AstrOsPacketType::COMMAND_RUN_ACK));
    EXPECT_EQ(AstrOsInterfaceResponseType::COMMAND_NAK, mapResponseType(AstrOsPacketType::COMMAND_RUN_NAK));
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
    auto pa = makePacket("id-aaaaaaaaaaaaaaaa", frag, AstrOsPacketType::CONFIG, 1, 2);
    auto pb_payload = std::string("piece-b");
    auto pb = makePacket("id-bbbbbbbbbbbbbbbb", pb_payload, AstrOsPacketType::CONFIG, 1, 2);

    auto r1 = AstrOsEspNowProtocol::extractPayload(pa, tracker, 1000);
    EXPECT_FALSE(r1.has_value());

    auto r2 = AstrOsEspNowProtocol::extractPayload(pb, tracker, 1000);
    EXPECT_FALSE(r2.has_value());
}

// ---------------- handleConfig ----------------

TEST(EspNowProtocol, HandleConfigValidProducesSetConfigMessage)
{
    auto tracker = PacketTracker();
    auto payload = joinUnits({"aa:bb:cc:dd:ee:ff", "msg-42", "controller-config-blob"});
    auto packet = makePacket("conf000000000000", payload, AstrOsPacketType::CONFIG);

    auto result = AstrOsEspNowProtocol::handleConfig(packet, tracker, 1000);

    ASSERT_EQ(AstrOsEspNowProtocol::HandlerStatus::Ok, result.status);
    ASSERT_TRUE(result.message.has_value());
    EXPECT_EQ(AstrOsInterfaceResponseType::SET_CONFIG, result.message->responseType);
    EXPECT_EQ("msg-42", result.message->msgId);
    EXPECT_EQ("", result.message->peerMac);
    EXPECT_EQ("", result.message->peerName);
    EXPECT_EQ("controller-config-blob", result.message->message);
}

TEST(EspNowProtocol, HandleConfigShortPayloadIsInvalid)
{
    auto tracker = PacketTracker();
    auto payload = joinUnits({"aa:bb:cc:dd:ee:ff", "msg-42"}); // missing message field
    auto packet = makePacket("conf000000000000", payload, AstrOsPacketType::CONFIG);

    auto result = AstrOsEspNowProtocol::handleConfig(packet, tracker, 1000);

    EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::InvalidPayload, result.status);
    EXPECT_FALSE(result.message.has_value());
    EXPECT_NE(std::string::npos, result.diagnostic.find("config"));
}

TEST(EspNowProtocol, HandleConfigMultiPacketPendingReturnsPending)
{
    auto tracker = PacketTracker();
    std::string frag = "first-half-";
    auto packet = makePacket("conf000000000000", frag, AstrOsPacketType::CONFIG, 1, 2);

    auto result = AstrOsEspNowProtocol::handleConfig(packet, tracker, 1000);

    EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::Pending, result.status);
    EXPECT_FALSE(result.message.has_value());
}

// ---------------- handleConfigAckNak ----------------

TEST(EspNowProtocol, HandleConfigAckNakAckValidProducesSendConfigAckMessage)
{
    auto payload = joinUnits({"aa:bb:cc:dd:ee:ff", "msg-42", "padawan-1", "ok"});
    auto packet = makePacket("conf000000000000", payload, AstrOsPacketType::CONFIG_ACK);

    auto result = AstrOsEspNowProtocol::handleConfigAckNak(packet);

    ASSERT_EQ(AstrOsEspNowProtocol::HandlerStatus::Ok, result.status);
    ASSERT_TRUE(result.message.has_value());
    EXPECT_EQ(AstrOsInterfaceResponseType::SEND_CONFIG_ACK, result.message->responseType);
    EXPECT_EQ("msg-42", result.message->msgId);
    EXPECT_EQ("aa:bb:cc:dd:ee:ff", result.message->peerMac);
    EXPECT_EQ("padawan-1", result.message->peerName);
    EXPECT_EQ("ok", result.message->message);
}

TEST(EspNowProtocol, HandleConfigAckNakNakMapsToSendConfigNak)
{
    auto payload = joinUnits({"aa:bb:cc:dd:ee:ff", "msg-42", "padawan-1", "bad-config"});
    auto packet = makePacket("conf000000000000", payload, AstrOsPacketType::CONFIG_NAK);

    auto result = AstrOsEspNowProtocol::handleConfigAckNak(packet);

    ASSERT_EQ(AstrOsEspNowProtocol::HandlerStatus::Ok, result.status);
    EXPECT_EQ(AstrOsInterfaceResponseType::SEND_CONFIG_NAK, result.message->responseType);
}

TEST(EspNowProtocol, HandleConfigAckNakShortPayloadIsInvalid)
{
    auto payload = joinUnits({"aa:bb:cc:dd:ee:ff", "msg-42", "padawan-1"}); // missing message
    auto packet = makePacket("conf000000000000", payload, AstrOsPacketType::CONFIG_ACK);

    auto result = AstrOsEspNowProtocol::handleConfigAckNak(packet);

    EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::InvalidPayload, result.status);
    EXPECT_NE(std::string::npos, result.diagnostic.find("config ack/nak"));
}

// ---------------- handleScriptDeploy ----------------

TEST(EspNowProtocol, HandleScriptDeployValidReconstructsScriptPayload)
{
    auto tracker = PacketTracker();
    auto payload = joinUnits({"aa:bb:cc:dd:ee:ff", "msg-42", "script-id-7", "0;1000;servo;ch1=12"});
    auto packet = makePacket("scrd000000000000", payload, AstrOsPacketType::SCRIPT_DEPLOY);

    auto result = AstrOsEspNowProtocol::handleScriptDeploy(packet, tracker, 1000);

    ASSERT_EQ(AstrOsEspNowProtocol::HandlerStatus::Ok, result.status);
    EXPECT_EQ(AstrOsInterfaceResponseType::SAVE_SCRIPT, result.message->responseType);
    EXPECT_EQ("msg-42", result.message->msgId);
    EXPECT_EQ(std::string("script-id-7") + UNIT_SEPARATOR + "0;1000;servo;ch1=12", result.message->message);
}

TEST(EspNowProtocol, HandleScriptDeployShortPayloadIsInvalid)
{
    auto tracker = PacketTracker();
    auto payload = joinUnits({"aa:bb:cc:dd:ee:ff", "msg-42", "script-id-7"}); // missing script
    auto packet = makePacket("scrd000000000000", payload, AstrOsPacketType::SCRIPT_DEPLOY);

    auto result = AstrOsEspNowProtocol::handleScriptDeploy(packet, tracker, 1000);

    EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::InvalidPayload, result.status);
    EXPECT_NE(std::string::npos, result.diagnostic.find("script deploy"));
}

TEST(EspNowProtocol, HandleScriptDeployMultiPacketPendingReturnsPending)
{
    auto tracker = PacketTracker();
    std::string frag = "first-half-";
    auto packet = makePacket("scrd000000000000", frag, AstrOsPacketType::SCRIPT_DEPLOY, 1, 2);

    auto result = AstrOsEspNowProtocol::handleScriptDeploy(packet, tracker, 1000);

    EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::Pending, result.status);
}

// ---------------- handleScriptRun ----------------

TEST(EspNowProtocol, HandleScriptRunValid)
{
    auto tracker = PacketTracker();
    auto payload = joinUnits({"aa:bb:cc:dd:ee:ff", "msg-42", "script-id-7"});
    auto packet = makePacket("scrr000000000000", payload, AstrOsPacketType::SCRIPT_RUN);

    auto result = AstrOsEspNowProtocol::handleScriptRun(packet, tracker, 1000);

    ASSERT_EQ(AstrOsEspNowProtocol::HandlerStatus::Ok, result.status);
    EXPECT_EQ(AstrOsInterfaceResponseType::SCRIPT_RUN, result.message->responseType);
    EXPECT_EQ("msg-42", result.message->msgId);
    EXPECT_EQ("script-id-7", result.message->message);
}

TEST(EspNowProtocol, HandleScriptRunShortPayloadIsInvalid)
{
    auto tracker = PacketTracker();
    auto payload = joinUnits({"aa:bb:cc:dd:ee:ff", "msg-42"});
    auto packet = makePacket("scrr000000000000", payload, AstrOsPacketType::SCRIPT_RUN);

    auto result = AstrOsEspNowProtocol::handleScriptRun(packet, tracker, 1000);

    EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::InvalidPayload, result.status);
    EXPECT_NE(std::string::npos, result.diagnostic.find("script run"));
}

// ---------------- handleCommandRun ----------------

TEST(EspNowProtocol, HandleCommandRunValid)
{
    auto tracker = PacketTracker();
    auto payload = joinUnits({"aa:bb:cc:dd:ee:ff", "msg-42", "command-body"});
    auto packet = makePacket("cmdr000000000000", payload, AstrOsPacketType::COMMAND_RUN);

    auto result = AstrOsEspNowProtocol::handleCommandRun(packet, tracker, 1000);

    ASSERT_EQ(AstrOsEspNowProtocol::HandlerStatus::Ok, result.status);
    EXPECT_EQ(AstrOsInterfaceResponseType::COMMAND, result.message->responseType);
    EXPECT_EQ("msg-42", result.message->msgId);
    EXPECT_EQ("command-body", result.message->message);
}

TEST(EspNowProtocol, HandleCommandRunShortPayloadIsInvalid)
{
    auto tracker = PacketTracker();
    auto payload = joinUnits({"aa:bb:cc:dd:ee:ff", "msg-42"});
    auto packet = makePacket("cmdr000000000000", payload, AstrOsPacketType::COMMAND_RUN);

    auto result = AstrOsEspNowProtocol::handleCommandRun(packet, tracker, 1000);

    EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::InvalidPayload, result.status);
    EXPECT_NE(std::string::npos, result.diagnostic.find("command run"));
}

// ---------------- handlePanicStop ----------------

TEST(EspNowProtocol, HandlePanicStopValid)
{
    auto tracker = PacketTracker();
    auto payload = joinUnits({"aa:bb:cc:dd:ee:ff", "msg-42", "PANIC"});
    auto packet = makePacket("panc000000000000", payload, AstrOsPacketType::PANIC_STOP);

    auto result = AstrOsEspNowProtocol::handlePanicStop(packet, tracker, 1000);

    ASSERT_EQ(AstrOsEspNowProtocol::HandlerStatus::Ok, result.status);
    EXPECT_EQ(AstrOsInterfaceResponseType::PANIC_STOP, result.message->responseType);
    EXPECT_EQ("msg-42", result.message->msgId);
    EXPECT_EQ("", result.message->message);
}

TEST(EspNowProtocol, HandlePanicStopShortPayloadIsInvalid)
{
    auto tracker = PacketTracker();
    auto payload = joinUnits({"aa:bb:cc:dd:ee:ff", "msg-42"});
    auto packet = makePacket("panc000000000000", payload, AstrOsPacketType::PANIC_STOP);

    auto result = AstrOsEspNowProtocol::handlePanicStop(packet, tracker, 1000);

    EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::InvalidPayload, result.status);
    EXPECT_NE(std::string::npos, result.diagnostic.find("panic stop"));
}

// ---------------- handleFormatSD ----------------

TEST(EspNowProtocol, HandleFormatSDValid)
{
    auto tracker = PacketTracker();
    auto payload = joinUnits({"aa:bb:cc:dd:ee:ff", "msg-42", "FORMATSD"});
    auto packet = makePacket("fmt0000000000000", payload, AstrOsPacketType::FORMAT_SD);

    auto result = AstrOsEspNowProtocol::handleFormatSD(packet, tracker, 1000);

    ASSERT_EQ(AstrOsEspNowProtocol::HandlerStatus::Ok, result.status);
    EXPECT_EQ(AstrOsInterfaceResponseType::FORMAT_SD, result.message->responseType);
    EXPECT_EQ("msg-42", result.message->msgId);
    EXPECT_EQ("", result.message->message);
}

TEST(EspNowProtocol, HandleFormatSDShortPayloadIsInvalid)
{
    auto tracker = PacketTracker();
    auto payload = joinUnits({"aa:bb:cc:dd:ee:ff", "msg-42"});
    auto packet = makePacket("fmt0000000000000", payload, AstrOsPacketType::FORMAT_SD);

    auto result = AstrOsEspNowProtocol::handleFormatSD(packet, tracker, 1000);

    EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::InvalidPayload, result.status);
    EXPECT_NE(std::string::npos, result.diagnostic.find("format sd"));
}

// ---------------- handleServoTest ----------------

TEST(EspNowProtocol, HandleServoTestValid)
{
    auto tracker = PacketTracker();
    auto payload = joinUnits({"aa:bb:cc:dd:ee:ff", "msg-42", "ch1=1500"});
    auto packet = makePacket("svts000000000000", payload, AstrOsPacketType::SERVO_TEST);

    auto result = AstrOsEspNowProtocol::handleServoTest(packet, tracker, 1000);

    ASSERT_EQ(AstrOsEspNowProtocol::HandlerStatus::Ok, result.status);
    EXPECT_EQ(AstrOsInterfaceResponseType::SERVO_TEST, result.message->responseType);
    EXPECT_EQ("msg-42", result.message->msgId);
    EXPECT_EQ("ch1=1500", result.message->message);
}

TEST(EspNowProtocol, HandleServoTestShortPayloadIsInvalid)
{
    auto tracker = PacketTracker();
    auto payload = joinUnits({"aa:bb:cc:dd:ee:ff", "msg-42"});
    auto packet = makePacket("svts000000000000", payload, AstrOsPacketType::SERVO_TEST);

    auto result = AstrOsEspNowProtocol::handleServoTest(packet, tracker, 1000);

    EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::InvalidPayload, result.status);
    EXPECT_NE(std::string::npos, result.diagnostic.find("servo test"));
}

// ---------------- handleBasicAckNak ----------------

TEST(EspNowProtocol, HandleBasicAckNakFourPartsProducesInterfaceMessage)
{
    auto payload = joinUnits({"aa:bb:cc:dd:ee:ff", "padawan-1", "msg-42", "details"});
    auto packet = makePacket("bank000000000000", payload, AstrOsPacketType::SCRIPT_DEPLOY_ACK);

    auto result = AstrOsEspNowProtocol::handleBasicAckNak(packet);

    ASSERT_EQ(AstrOsEspNowProtocol::HandlerStatus::Ok, result.status);
    EXPECT_EQ(AstrOsInterfaceResponseType::SAVE_SCRIPT_ACK, result.message->responseType);
    EXPECT_EQ("msg-42", result.message->msgId);
    EXPECT_EQ("aa:bb:cc:dd:ee:ff", result.message->peerMac);
    EXPECT_EQ("padawan-1", result.message->peerName);
    EXPECT_EQ("details", result.message->message);
}

TEST(EspNowProtocol, HandleBasicAckNakThreePartsDefaultsMessageToNa)
{
    auto payload = joinUnits({"aa:bb:cc:dd:ee:ff", "padawan-1", "msg-42"});
    auto packet = makePacket("bank000000000000", payload, AstrOsPacketType::SERVO_TEST_ACK);

    auto result = AstrOsEspNowProtocol::handleBasicAckNak(packet);

    ASSERT_EQ(AstrOsEspNowProtocol::HandlerStatus::Ok, result.status);
    EXPECT_EQ("na", result.message->message);
}

TEST(EspNowProtocol, HandleBasicAckNakShortPayloadIsInvalid)
{
    auto payload = joinUnits({"aa:bb:cc:dd:ee:ff", "padawan-1"});
    auto packet = makePacket("bank000000000000", payload, AstrOsPacketType::SCRIPT_DEPLOY_NAK);

    auto result = AstrOsEspNowProtocol::handleBasicAckNak(packet);

    EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::InvalidPayload, result.status);
    EXPECT_NE(std::string::npos, result.diagnostic.find("basic ack/nak"));
}

// ---------------- handlePacket (dispatcher) ----------------

TEST(EspNowProtocol, DispatcherRoutesSingleRecordTypesToTheirHandlers)
{
    auto tracker = PacketTracker();
    struct Case
    {
        AstrOsPacketType in;
        AstrOsInterfaceResponseType expected;
    };
    const Case cases[] = {
        {AstrOsPacketType::CONFIG, AstrOsInterfaceResponseType::SET_CONFIG},
        {AstrOsPacketType::SCRIPT_RUN, AstrOsInterfaceResponseType::SCRIPT_RUN},
        {AstrOsPacketType::PANIC_STOP, AstrOsInterfaceResponseType::PANIC_STOP},
        {AstrOsPacketType::FORMAT_SD, AstrOsInterfaceResponseType::FORMAT_SD},
        {AstrOsPacketType::COMMAND_RUN, AstrOsInterfaceResponseType::COMMAND},
        {AstrOsPacketType::SERVO_TEST, AstrOsInterfaceResponseType::SERVO_TEST},
    };

    for (const auto &c : cases)
    {
        auto payload = joinUnits({"aa:bb:cc:dd:ee:ff", "msg-42", "body"});
        auto packet = makePacket("disp000000000000", payload, c.in);

        auto result = AstrOsEspNowProtocol::handlePacket(packet, tracker, false, 1000);

        ASSERT_EQ(AstrOsEspNowProtocol::HandlerStatus::Ok, result.status) << "packetType=" << static_cast<int>(c.in);
        EXPECT_EQ(c.expected, result.message->responseType) << "packetType=" << static_cast<int>(c.in);
    }
}

TEST(EspNowProtocol, DispatcherRoutesAckNakTypesToBasicAckNakHandler)
{
    auto tracker = PacketTracker();
    auto payload = joinUnits({"aa:bb:cc:dd:ee:ff", "padawan-1", "msg-42", "ok"});

    struct Case
    {
        AstrOsPacketType in;
        AstrOsInterfaceResponseType expected;
    };
    // SERVO_TEST_ACK is dispatched to handleBasicAckNak but mapResponseType
    // returns UNKNOWN for it. This is intentional: padawans do not ACK
    // SERVO_TEST packets because the control UI drives servos via sliders,
    // which produces one request per tick — an ACK storm the mesh can't
    // absorb. The dispatch case is retained for wire-format completeness.
    const Case cases[] = {
        {AstrOsPacketType::SCRIPT_DEPLOY_ACK, AstrOsInterfaceResponseType::SAVE_SCRIPT_ACK},
        {AstrOsPacketType::SCRIPT_DEPLOY_NAK, AstrOsInterfaceResponseType::SAVE_SCRIPT_NAK},
        {AstrOsPacketType::SCRIPT_RUN_ACK, AstrOsInterfaceResponseType::SCRIPT_RUN_ACK},
        {AstrOsPacketType::SCRIPT_RUN_NAK, AstrOsInterfaceResponseType::SCRIPT_RUN_NAK},
        {AstrOsPacketType::FORMAT_SD_ACK, AstrOsInterfaceResponseType::FORMAT_SD_ACK},
        {AstrOsPacketType::FORMAT_SD_NAK, AstrOsInterfaceResponseType::FORMAT_SD_NAK},
        {AstrOsPacketType::COMMAND_RUN_ACK, AstrOsInterfaceResponseType::COMMAND_ACK},
        {AstrOsPacketType::COMMAND_RUN_NAK, AstrOsInterfaceResponseType::COMMAND_NAK},
        {AstrOsPacketType::SERVO_TEST_ACK, AstrOsInterfaceResponseType::UNKNOWN},
    };

    for (const auto &c : cases)
    {
        auto packet = makePacket("disp000000000000", payload, c.in);

        auto result = AstrOsEspNowProtocol::handlePacket(packet, tracker, true, 1000);

        ASSERT_EQ(AstrOsEspNowProtocol::HandlerStatus::Ok, result.status) << "packetType=" << static_cast<int>(c.in);
        EXPECT_EQ(c.expected, result.message->responseType) << "packetType=" << static_cast<int>(c.in);
    }
}

TEST(EspNowProtocol, DispatcherRoutesConfigAckNakToConfigAckNakHandler)
{
    auto tracker = PacketTracker();
    auto payload = joinUnits({"aa:bb:cc:dd:ee:ff", "msg-42", "padawan-1", "body"});

    auto ack = makePacket("disp000000000000", payload, AstrOsPacketType::CONFIG_ACK);
    auto ackResult = AstrOsEspNowProtocol::handlePacket(ack, tracker, true, 1000);
    ASSERT_EQ(AstrOsEspNowProtocol::HandlerStatus::Ok, ackResult.status);
    EXPECT_EQ(AstrOsInterfaceResponseType::SEND_CONFIG_ACK, ackResult.message->responseType);

    auto nak = makePacket("disp000000000000", payload, AstrOsPacketType::CONFIG_NAK);
    auto nakResult = AstrOsEspNowProtocol::handlePacket(nak, tracker, true, 1000);
    ASSERT_EQ(AstrOsEspNowProtocol::HandlerStatus::Ok, nakResult.status);
    EXPECT_EQ(AstrOsInterfaceResponseType::SEND_CONFIG_NAK, nakResult.message->responseType);
}

TEST(EspNowProtocol, DispatcherReturnsUnsupportedTypeForDeferredTypesInCorrectRole)
{
    auto tracker = PacketTracker();
    std::string empty;

    // Master-only types, master role -> UnsupportedType (Phase 2 handles).
    for (auto type :
         {AstrOsPacketType::REGISTRATION_REQ, AstrOsPacketType::REGISTRATION_ACK, AstrOsPacketType::POLL_ACK})
    {
        auto packet = makePacket("disp000000000000", empty, type);
        auto result = AstrOsEspNowProtocol::handlePacket(packet, tracker, true, 1000);
        EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::UnsupportedType, result.status)
            << "packetType=" << static_cast<int>(type);
    }

    // Padawan-only types, padawan role -> UnsupportedType.
    for (auto type : {AstrOsPacketType::REGISTRATION, AstrOsPacketType::POLL})
    {
        auto packet = makePacket("disp000000000000", empty, type);
        auto result = AstrOsEspNowProtocol::handlePacket(packet, tracker, false, 1000);
        EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::UnsupportedType, result.status)
            << "packetType=" << static_cast<int>(type);
    }
}

TEST(EspNowProtocol, DispatcherReturnsWrongRoleForDeferredTypesInOppositeRole)
{
    auto tracker = PacketTracker();
    std::string empty;

    // Master-only types received by a padawan.
    for (auto type :
         {AstrOsPacketType::REGISTRATION_REQ, AstrOsPacketType::REGISTRATION_ACK, AstrOsPacketType::POLL_ACK})
    {
        auto packet = makePacket("disp000000000000", empty, type);
        auto result = AstrOsEspNowProtocol::handlePacket(packet, tracker, false, 1000);
        EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::WrongRole, result.status)
            << "packetType=" << static_cast<int>(type);
    }

    // Padawan-only types received by the master.
    for (auto type : {AstrOsPacketType::REGISTRATION, AstrOsPacketType::POLL})
    {
        auto packet = makePacket("disp000000000000", empty, type);
        auto result = AstrOsEspNowProtocol::handlePacket(packet, tracker, true, 1000);
        EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::WrongRole, result.status)
            << "packetType=" << static_cast<int>(type);
    }
}

TEST(EspNowProtocol, DispatcherReturnsUnknownTypeForUnknownPacketType)
{
    auto tracker = PacketTracker();
    std::string empty;
    auto packet = makePacket("disp000000000000", empty, AstrOsPacketType::UNKNOWN);

    auto result = AstrOsEspNowProtocol::handlePacket(packet, tracker, true, 1000);

    EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::UnknownType, result.status);
}

TEST(EspNowProtocol, DispatcherPropagatesPendingFromMultiPacketHandlers)
{
    auto tracker = PacketTracker();
    std::string frag = "first-half-";
    auto packet = makePacket("disp000000000000", frag, AstrOsPacketType::CONFIG, 1, 2);

    auto result = AstrOsEspNowProtocol::handlePacket(packet, tracker, false, 1000);

    EXPECT_EQ(AstrOsEspNowProtocol::HandlerStatus::Pending, result.status);
}

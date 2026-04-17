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

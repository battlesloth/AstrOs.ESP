#include <AstrOsEspNowProtocol.hpp>
#include <AstrOsStringUtils.hpp>

#include <sstream>

namespace AstrOsEspNowProtocol
{
    namespace
    {
        HandlerResult invalid(const std::string &label, const std::string &payload)
        {
            return {HandlerStatus::InvalidPayload, std::nullopt, "Invalid " + label + " payload: " + payload};
        }

        HandlerResult pending()
        {
            return {HandlerStatus::Pending, std::nullopt, ""};
        }

        HandlerResult ok(InterfaceMessage message)
        {
            return {HandlerStatus::Ok, std::move(message), ""};
        }
    } // namespace

    AstrOsInterfaceResponseType mapResponseType(AstrOsPacketType packetType)
    {
        switch (packetType)
        {
        case AstrOsPacketType::CONFIG_ACK:
            return AstrOsInterfaceResponseType::SEND_CONFIG_ACK;
        case AstrOsPacketType::CONFIG_NAK:
            return AstrOsInterfaceResponseType::SEND_CONFIG_NAK;
        case AstrOsPacketType::SCRIPT_DEPLOY_ACK:
            return AstrOsInterfaceResponseType::SAVE_SCRIPT_ACK;
        case AstrOsPacketType::SCRIPT_DEPLOY_NAK:
            return AstrOsInterfaceResponseType::SAVE_SCRIPT_NAK;
        case AstrOsPacketType::SCRIPT_RUN_ACK:
            return AstrOsInterfaceResponseType::SCRIPT_RUN_ACK;
        case AstrOsPacketType::SCRIPT_RUN_NAK:
            return AstrOsInterfaceResponseType::SCRIPT_RUN_NAK;
        case AstrOsPacketType::FORMAT_SD_ACK:
            return AstrOsInterfaceResponseType::FORMAT_SD_ACK;
        case AstrOsPacketType::FORMAT_SD_NAK:
            return AstrOsInterfaceResponseType::FORMAT_SD_NAK;
        default:
            return AstrOsInterfaceResponseType::UNKNOWN;
        }
    }

    std::optional<std::string> extractPayload(const astros_packet_t &packet, PacketTracker &tracker, int nowMs)
    {
        if (packet.totalPackets > 1)
        {
            auto msgId = std::string(reinterpret_cast<const char *>(packet.id), sizeof(packet.id));
            auto fragment = std::string(reinterpret_cast<const char *>(packet.payload), packet.payloadSize);

            PacketData data{packet.packetNumber, packet.totalPackets, fragment};

            auto result = tracker.addPacket(msgId, data, nowMs);
            if (result == AddPacketResult::MESSAGE_COMPLETE)
            {
                return tracker.getMessage(msgId);
            }
            return std::nullopt;
        }

        return std::string(reinterpret_cast<const char *>(packet.payload), packet.payloadSize);
    }

    HandlerResult handleConfig(const astros_packet_t &packet, PacketTracker &tracker, int nowMs)
    {
        auto payload = extractPayload(packet, tracker, nowMs);
        if (!payload)
        {
            return pending();
        }

        // 0 = dest mac, 1 = origination msgId, 2 = config message
        auto parts = AstrOsStringUtils::splitString(*payload, UNIT_SEPARATOR);
        if (parts.size() < 3)
        {
            return invalid("config", *payload);
        }

        return ok(InterfaceMessage{AstrOsInterfaceResponseType::SET_CONFIG, parts[1], "", "", parts[2]});
    }

    HandlerResult handleConfigAckNak(const astros_packet_t &packet)
    {
        auto payload = std::string(reinterpret_cast<const char *>(packet.payload), packet.payloadSize);

        // 0 = peer mac, 1 = origination msgId, 2 = peer name, 3 = message
        auto parts = AstrOsStringUtils::splitString(payload, UNIT_SEPARATOR);
        if (parts.size() < 4)
        {
            return invalid("config ack/nak", payload);
        }

        return ok(InterfaceMessage{mapResponseType(packet.packetType), parts[1], parts[0], parts[2], parts[3]});
    }

    HandlerResult handleScriptDeploy(const astros_packet_t &packet, PacketTracker &tracker, int nowMs)
    {
        auto payload = extractPayload(packet, tracker, nowMs);
        if (!payload)
        {
            return pending();
        }

        // 0 = dest mac, 1 = origination msgId, 2 = scriptId, 3 = script
        auto parts = AstrOsStringUtils::splitString(*payload, UNIT_SEPARATOR);
        if (parts.size() < 4)
        {
            return invalid("script deploy", *payload);
        }

        std::stringstream ss;
        ss << parts[2] << UNIT_SEPARATOR << parts[3];

        return ok(InterfaceMessage{AstrOsInterfaceResponseType::SAVE_SCRIPT, parts[1], "", "", ss.str()});
    }

    HandlerResult handleScriptRun(const astros_packet_t &packet, PacketTracker &tracker, int nowMs)
    {
        auto payload = extractPayload(packet, tracker, nowMs);
        if (!payload)
        {
            return pending();
        }

        // 0 = dest mac, 1 = origination msgId, 2 = scriptId
        auto parts = AstrOsStringUtils::splitString(*payload, UNIT_SEPARATOR);
        if (parts.size() < 3)
        {
            return invalid("script run", *payload);
        }

        return ok(InterfaceMessage{AstrOsInterfaceResponseType::SCRIPT_RUN, parts[1], "", "", parts[2]});
    }

    HandlerResult handleCommandRun(const astros_packet_t &packet, PacketTracker &tracker, int nowMs)
    {
        auto payload = extractPayload(packet, tracker, nowMs);
        if (!payload)
        {
            return pending();
        }

        // 0 = dest mac, 1 = origination msgId, 2 = command
        auto parts = AstrOsStringUtils::splitString(*payload, UNIT_SEPARATOR);
        if (parts.size() < 3)
        {
            return invalid("command run", *payload);
        }

        return ok(InterfaceMessage{AstrOsInterfaceResponseType::COMMAND, parts[1], "", "", parts[2]});
    }

    HandlerResult handlePanicStop(const astros_packet_t &packet, PacketTracker &tracker, int nowMs)
    {
        auto payload = extractPayload(packet, tracker, nowMs);
        if (!payload)
        {
            return pending();
        }

        // 0 = dest mac, 1 = origination msgId, 2 = literal "PANIC" sentinel
        // (appended by the master in main.cpp SEND_PANIC_STOP; forwarded unread).
        auto parts = AstrOsStringUtils::splitString(*payload, UNIT_SEPARATOR);
        if (parts.size() < 3)
        {
            return invalid("panic stop", *payload);
        }

        return ok(InterfaceMessage{AstrOsInterfaceResponseType::PANIC_STOP, parts[1], "", "", ""});
    }

    HandlerResult handleFormatSD(const astros_packet_t &packet, PacketTracker &tracker, int nowMs)
    {
        auto payload = extractPayload(packet, tracker, nowMs);
        if (!payload)
        {
            return pending();
        }

        // 0 = dest mac, 1 = origination msgId, 2 = literal "FORMATSD" sentinel
        // (appended by the master in main.cpp SEND_FORMAT_SD; forwarded unread).
        auto parts = AstrOsStringUtils::splitString(*payload, UNIT_SEPARATOR);
        if (parts.size() < 3)
        {
            return invalid("format sd", *payload);
        }

        return ok(InterfaceMessage{AstrOsInterfaceResponseType::FORMAT_SD, parts[1], "", "", ""});
    }

    HandlerResult handleServoTest(const astros_packet_t &packet, PacketTracker &tracker, int nowMs)
    {
        auto payload = extractPayload(packet, tracker, nowMs);
        if (!payload)
        {
            return pending();
        }

        // 0 = dest mac, 1 = origination msgId, 2 = servo-test spec
        auto parts = AstrOsStringUtils::splitString(*payload, UNIT_SEPARATOR);
        if (parts.size() < 3)
        {
            return invalid("servo test", *payload);
        }

        return ok(InterfaceMessage{AstrOsInterfaceResponseType::SERVO_TEST, parts[1], "", "", parts[2]});
    }

    HandlerResult handlePacket(const astros_packet_t & /*packet*/, PacketTracker & /*tracker*/, bool /*isMasterNode*/,
                               int /*nowMs*/)
    {
        return HandlerResult{HandlerStatus::UnknownType, std::nullopt, ""};
    }

} // namespace AstrOsEspNowProtocol

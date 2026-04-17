#include <AstrOsEspNowProtocol.hpp>

namespace AstrOsEspNowProtocol
{
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

    HandlerResult handlePacket(const astros_packet_t & /*packet*/, PacketTracker & /*tracker*/, bool /*isMasterNode*/,
                               int /*nowMs*/)
    {
        return HandlerResult{HandlerStatus::UnknownType, std::nullopt, ""};
    }

} // namespace AstrOsEspNowProtocol

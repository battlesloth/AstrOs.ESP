#include <AstrOsEspNowProtocol.hpp>

namespace AstrOsEspNowProtocol
{
    HandlerResult handlePacket(const astros_packet_t & /*packet*/, PacketTracker & /*tracker*/, bool /*isMasterNode*/)
    {
        return HandlerResult{HandlerStatus::UnknownType, std::nullopt, ""};
    }

    AstrOsInterfaceResponseType mapResponseType(AstrOsPacketType /*packetType*/)
    {
        return AstrOsInterfaceResponseType::UNKNOWN;
    }

} // namespace AstrOsEspNowProtocol

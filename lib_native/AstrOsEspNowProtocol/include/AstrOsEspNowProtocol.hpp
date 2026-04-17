#pragma once

#include <optional>
#include <string>

#include <AstrOsInterfaceResponseMsg.hpp>
#include <AstrOsMessaging.hpp>
#include <PacketTracker.hpp>

namespace AstrOsEspNowProtocol
{
    struct InterfaceMessage
    {
        AstrOsInterfaceResponseType responseType = AstrOsInterfaceResponseType::UNKNOWN;
        std::string msgId;
        std::string peerMac;
        std::string peerName;
        std::string message;
    };

    enum class HandlerStatus
    {
        Ok,              // handler produced an InterfaceMessage
        Pending,         // multi-packet message awaiting more fragments
        InvalidPayload,  // malformed (wrong part count, empty field, etc.)
        WrongRole,       // packet addressed to a role this node does not hold
        UnsupportedType, // packet type not yet extracted; adapter handles it
        UnknownType,     // packetType out of range
    };

    struct HandlerResult
    {
        HandlerStatus status = HandlerStatus::UnknownType;
        std::optional<InterfaceMessage> message;
        std::string diagnostic;
    };

    // Decodes an already-parsed, already-validated ESP-NOW packet.
    // Returns an InterfaceMessage for the MIXED adapter to forward to
    // its interface queue, or a Pending/error status with a diagnostic.
    // `tracker` is mutated for multi-packet messages.
    HandlerResult handlePacket(const astros_packet_t &packet, PacketTracker &tracker, bool isMasterNode);

    // Maps a packet type to the interface-response type used when a
    // handler succeeds. Returns UNKNOWN for types this phase does not
    // handle (registration, poll, poll-ack).
    AstrOsInterfaceResponseType mapResponseType(AstrOsPacketType packetType);

} // namespace AstrOsEspNowProtocol

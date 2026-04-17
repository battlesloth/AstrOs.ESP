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
    // `tracker` is mutated for multi-packet messages; `nowMs` is the
    // current monotonic time in milliseconds (on-target: esp_timer_get_time() / 1000).
    HandlerResult handlePacket(const astros_packet_t &packet, PacketTracker &tracker, bool isMasterNode, int nowMs);

    // Maps a packet type to the interface-response type used when a
    // handler succeeds. Returns UNKNOWN for types this phase does not
    // handle (registration, poll, poll-ack).
    AstrOsInterfaceResponseType mapResponseType(AstrOsPacketType packetType);

    // Returns the assembled payload for `packet`, or std::nullopt when
    // multi-packet reassembly is still pending. Single-packet messages
    // return immediately. Multi-packet messages mutate `tracker`.
    std::optional<std::string> extractPayload(const astros_packet_t &packet, PacketTracker &tracker, int nowMs);

    // Individual packet-type handlers. Exposed for direct unit testing;
    // also used by the `handlePacket` dispatcher. `tracker` and `nowMs`
    // are ignored by handlers that never span multiple packets (the
    // ack/nak handlers), but are kept in the signature so the dispatcher
    // can invoke any handler uniformly.
    HandlerResult handleConfig(const astros_packet_t &packet, PacketTracker &tracker, int nowMs);
    HandlerResult handleConfigAckNak(const astros_packet_t &packet);
    HandlerResult handleScriptDeploy(const astros_packet_t &packet, PacketTracker &tracker, int nowMs);
    HandlerResult handleScriptRun(const astros_packet_t &packet, PacketTracker &tracker, int nowMs);
    HandlerResult handleCommandRun(const astros_packet_t &packet, PacketTracker &tracker, int nowMs);
    HandlerResult handlePanicStop(const astros_packet_t &packet, PacketTracker &tracker, int nowMs);
    HandlerResult handleFormatSD(const astros_packet_t &packet, PacketTracker &tracker, int nowMs);
    HandlerResult handleServoTest(const astros_packet_t &packet, PacketTracker &tracker, int nowMs);
    HandlerResult handleBasicAckNak(const astros_packet_t &packet);

} // namespace AstrOsEspNowProtocol

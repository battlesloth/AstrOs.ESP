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

    // ─── OTA records (M1 wire format) ────────────────────────────────────
    //
    // Each parser takes a parsed astros_packet_t (output of
    // AstrOsEspNowMessageService::parsePacket) and returns a POD record
    // with a `bool valid` field. valid=false means the payload length
    // didn't match the spec'd size, or a reason/status enum value was
    // out of range.
    //
    // These parsers are PURE — they do not mutate any state, do not
    // allocate, and operate only on the bytes already in the packet
    // buffer. The future MIXED layer (M3/M4) calls them from its ESP-NOW
    // RX callback to convert wire bytes into records before queueing.

    struct OtaBeginRecord
    {
        uint8_t xferId = 0;
        uint32_t totalSize = 0;
        uint16_t chunkSize = 0;
        uint32_t totalChunks = 0;
        uint8_t sha256Expected[32] = {0};
        uint8_t flags = 0;
        bool valid = false;
    };

    struct OtaBeginAckRecord
    {
        uint8_t xferId = 0;
        bool valid = false;
    };

    struct OtaBeginNakRecord
    {
        uint8_t xferId = 0;
        OtaBeginNakReason reason = OtaBeginNakReason::BUSY;
        bool valid = false;
    };

    // OtaDataRecord.payload is a pointer into the original parsed packet's
    // buffer. The caller MUST consume or copy the bytes before the buffer
    // is freed/reused. payloadLen is the spec'd length from the header;
    // parseOtaData rejects records where payloadLen doesn't match the
    // actual bytes-after-header count.
    struct OtaDataRecord
    {
        uint8_t xferId = 0;
        uint32_t seq = 0;
        uint16_t payloadLen = 0;
        uint16_t crc16 = 0;
        const uint8_t *payload = nullptr;
        bool valid = false;
    };

    struct OtaDataAckRecord
    {
        uint8_t xferId = 0;
        uint32_t highestContiguousSeq = 0;
        uint32_t nextExpectedSeq = 0;
        uint8_t windowRemaining = 0;
        bool valid = false;
    };

    struct OtaDataNakRecord
    {
        uint8_t xferId = 0;
        uint32_t highestContiguousSeq = 0;
        uint32_t nextExpectedSeq = 0;
        uint8_t windowRemaining = 0;
        OtaDataNakReason reason = OtaDataNakReason::NONE;
        bool valid = false;
    };

    struct OtaEndRecord
    {
        uint8_t xferId = 0;
        uint32_t totalChunksSent = 0;
        uint8_t sha256Final[32] = {0};
        bool valid = false;
    };

    struct OtaEndAckRecord
    {
        uint8_t xferId = 0;
        OtaEndStatus status = OtaEndStatus::OK;
        uint8_t sha256Computed[32] = {0};
        bool valid = false;
    };

    OtaBeginRecord parseOtaBegin(const astros_packet_t &packet);
    OtaBeginAckRecord parseOtaBeginAck(const astros_packet_t &packet);
    OtaBeginNakRecord parseOtaBeginNak(const astros_packet_t &packet);
    OtaDataRecord parseOtaData(const astros_packet_t &packet);
    OtaDataAckRecord parseOtaDataAck(const astros_packet_t &packet);
    OtaDataNakRecord parseOtaDataNak(const astros_packet_t &packet);
    OtaEndRecord parseOtaEnd(const astros_packet_t &packet);
    OtaEndAckRecord parseOtaEndAck(const astros_packet_t &packet);

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

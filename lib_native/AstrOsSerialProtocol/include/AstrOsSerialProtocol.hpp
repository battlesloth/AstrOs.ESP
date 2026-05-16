#pragma once

#include <string>
#include <vector>

#include <AstrOsInterfaceResponseMsg.hpp>
#include <AstrOsSerialMessageService.hpp>

namespace AstrOsSerialProtocol
{
    enum class DecodeRejectReason
    {
        WRONG_PART_COUNT,
        EMPTY_DEST,
        EMPTY_VALUE,
        UNKNOWN_TYPE,
        EMPTY_PAYLOAD,
    };

    struct DecodedCommand
    {
        AstrOsInterfaceResponseType responseType = AstrOsInterfaceResponseType::UNKNOWN;
        std::string msgId;
        std::string peerMac;  // empty string = broadcast
        std::string peerName; // always empty today; kept for future peer-name routing
        std::string message;
    };

    struct DecodeReject
    {
        std::string entry;
        DecodeRejectReason reason = DecodeRejectReason::WRONG_PART_COUNT;
    };

    struct DecodeResult
    {
        std::vector<DecodedCommand> commands;
        std::vector<DecodeReject> rejects;
    };

    // Decodes an already-validated serial message. All three inputs —
    // `type`, `msgId`, and `payload` — come from the fields populated by
    // AstrOsSerialMessageService::validateSerialMsg. `payload` is the
    // group after GROUP_SEPARATOR (the RECORD_SEPARATOR-joined controller
    // records) and is empty for messages that carry no payload, e.g.
    // REGISTRATION_SYNC.
    //
    // Pure: no I/O, no allocations outside the returned vectors, no
    // FreeRTOS/ESP dependencies.
    DecodeResult decodeSerialMessage(AstrOsSerialMessageType type, const std::string &msgId,
                                     const std::string &payload);

    // Master-vs-padawan response-type lookup. Returns UNKNOWN for any
    // (type, isMaster) combination the handler does not forward — this
    // matches the previous private getResponseType() behaviour exactly.
    AstrOsInterfaceResponseType mapResponseType(AstrOsSerialMessageType type, bool isMaster);

    // Human-readable label for a reject reason, suitable for use as a
    // format argument (e.g. ESP_LOGW(TAG, "Invalid %s: %s", ...)). Strings
    // are process-lifetime; callers must not free them.
    const char *describeRejectReason(DecodeRejectReason reason);

    // Ceil-divide for OTA totalChunks computation. Ceil (not floor) because
    // the final chunk is short when totalSize isn't a multiple of chunkSize.
    // Returns 0 on either zero input (chunkSize=0 is a defensive guard
    // against div-by-zero; callers reject upstream).
    uint32_t chunksForSize(uint32_t totalSize, uint16_t chunkSize);
} // namespace AstrOsSerialProtocol

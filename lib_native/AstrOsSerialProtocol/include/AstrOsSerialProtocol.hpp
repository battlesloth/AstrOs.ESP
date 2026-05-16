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

    // Ceil-divide totalSize by chunkSize for OTA transfers. Used to compute
    // the receiver-side totalChunks from the server's declared totalSize.
    // The ceil (not floor) is correctness-critical: the final chunk is short
    // whenever totalSize is not an exact multiple of chunkSize.
    //
    // Returns 0 when either input is 0. The chunkSize=0 path is a defensive
    // guard — callers (e.g. parseFwChunk) reject upstream, but a future caller
    // that forgets would otherwise divide by zero.
    uint32_t chunksForSize(uint32_t totalSize, uint16_t chunkSize);
} // namespace AstrOsSerialProtocol

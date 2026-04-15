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

    // Decodes an already-validated serial message payload. `type` and
    // `msgId` come from AstrOsSerialMessageService::validateSerialMsg;
    // `message` is the full raw message (the decoder re-splits on
    // GROUP_SEPARATOR internally). `isMaster` routes each command through
    // the master-side or padawan-side response-type table.
    //
    // Pure: no I/O, no allocations outside the returned vectors, no
    // FreeRTOS/ESP dependencies.
    DecodeResult decodeSerialMessage(AstrOsSerialMessageType type, const std::string &msgId, const std::string &message,
                                     bool isMaster);

    // Master-vs-padawan response-type lookup. Returns UNKNOWN for any
    // (type, isMaster) combination the handler does not forward — this
    // matches the previous private getResponseType() behaviour exactly.
    AstrOsInterfaceResponseType mapResponseType(AstrOsSerialMessageType type, bool isMaster);

    // Human-readable label for a reject reason, suitable for use as a
    // format argument (e.g. ESP_LOGW(TAG, "Invalid %s: %s", ...)). Strings
    // are process-lifetime; callers must not free them.
    const char *describeRejectReason(DecodeRejectReason reason);
} // namespace AstrOsSerialProtocol

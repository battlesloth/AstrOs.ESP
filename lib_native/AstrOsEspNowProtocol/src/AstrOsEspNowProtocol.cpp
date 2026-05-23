#include <AstrOsEspNowProtocol.hpp>
#include <AstrOsStringUtils.hpp>

#include <cstring>
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
        case AstrOsPacketType::COMMAND_RUN_ACK:
            return AstrOsInterfaceResponseType::COMMAND_ACK;
        case AstrOsPacketType::COMMAND_RUN_NAK:
            return AstrOsInterfaceResponseType::COMMAND_NAK;
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

    HandlerResult handleBasicAckNak(const astros_packet_t &packet)
    {
        auto payload = std::string(reinterpret_cast<const char *>(packet.payload), packet.payloadSize);

        // 0 = peer mac, 1 = peer name, 2 = origination msgId, 3 = optional message (default "na")
        auto parts = AstrOsStringUtils::splitString(payload, UNIT_SEPARATOR);
        if (parts.size() < 3)
        {
            return invalid("basic ack/nak", payload);
        }

        std::string message = parts.size() > 3 ? parts[3] : std::string("na");

        return ok(InterfaceMessage{mapResponseType(packet.packetType), parts[2], parts[0], parts[1], message});
    }

    namespace
    {
        HandlerResult unsupportedOrWrongRole(bool roleMatches)
        {
            return {roleMatches ? HandlerStatus::UnsupportedType : HandlerStatus::WrongRole, std::nullopt, ""};
        }
    } // namespace

    HandlerResult handlePacket(const astros_packet_t &packet, PacketTracker &tracker, bool isMasterNode, int nowMs)
    {
        switch (packet.packetType)
        {
        // Peer-state-entangled handlers, deferred to Phase 2. The dispatcher
        // still enforces role gating here so the MIXED adapter can rely on a
        // single rule: UnsupportedType means "fall through to residual switch".
        case AstrOsPacketType::REGISTRATION_REQ:
        case AstrOsPacketType::REGISTRATION_ACK:
        case AstrOsPacketType::POLL_ACK:
            return unsupportedOrWrongRole(isMasterNode);
        case AstrOsPacketType::REGISTRATION:
        case AstrOsPacketType::POLL:
            return unsupportedOrWrongRole(!isMasterNode);

        // Single-record handlers extracted in Phase 1.
        case AstrOsPacketType::CONFIG:
            return handleConfig(packet, tracker, nowMs);
        case AstrOsPacketType::CONFIG_ACK:
        case AstrOsPacketType::CONFIG_NAK:
            return handleConfigAckNak(packet);
        case AstrOsPacketType::SCRIPT_DEPLOY:
            return handleScriptDeploy(packet, tracker, nowMs);
        case AstrOsPacketType::SCRIPT_RUN:
            return handleScriptRun(packet, tracker, nowMs);
        case AstrOsPacketType::PANIC_STOP:
            return handlePanicStop(packet, tracker, nowMs);
        case AstrOsPacketType::FORMAT_SD:
            return handleFormatSD(packet, tracker, nowMs);
        case AstrOsPacketType::COMMAND_RUN:
            return handleCommandRun(packet, tracker, nowMs);
        case AstrOsPacketType::SERVO_TEST:
            return handleServoTest(packet, tracker, nowMs);

        case AstrOsPacketType::SCRIPT_DEPLOY_ACK:
        case AstrOsPacketType::SCRIPT_DEPLOY_NAK:
        case AstrOsPacketType::SCRIPT_RUN_ACK:
        case AstrOsPacketType::SCRIPT_RUN_NAK:
        case AstrOsPacketType::FORMAT_SD_ACK:
        case AstrOsPacketType::FORMAT_SD_NAK:
        case AstrOsPacketType::COMMAND_RUN_ACK:
        case AstrOsPacketType::COMMAND_RUN_NAK:
        case AstrOsPacketType::SERVO_TEST_ACK:
            return handleBasicAckNak(packet);

        default:
            return {HandlerStatus::UnknownType, std::nullopt, ""};
        }
    }

    OtaBeginRecord parseOtaBegin(const astros_packet_t &packet)
    {
        OtaBeginRecord rec;
        if (packet.packetType != AstrOsPacketType::OTA_BEGIN ||
            packet.payloadSize != static_cast<int>(sizeof(OtaBeginPayload)))
        {
            return rec; // valid stays false
        }
        const auto *p = reinterpret_cast<const OtaBeginPayload *>(packet.payload);
        rec.xferId = p->xferId;
        rec.totalSize = p->totalSize;
        rec.chunkSize = p->chunkSize;
        rec.totalChunks = p->totalChunks;
        std::memcpy(rec.sha256Expected, p->sha256Expected, 32);
        rec.flags = p->flags;
        rec.valid = true;
        return rec;
    }

    OtaBeginAckRecord parseOtaBeginAck(const astros_packet_t &packet)
    {
        OtaBeginAckRecord rec;
        if (packet.packetType != AstrOsPacketType::OTA_BEGIN_ACK ||
            packet.payloadSize != static_cast<int>(sizeof(OtaBeginAckPayload)))
        {
            return rec;
        }
        const auto *p = reinterpret_cast<const OtaBeginAckPayload *>(packet.payload);
        rec.xferId = p->xferId;
        rec.valid = true;
        return rec;
    }

    OtaBeginNakRecord parseOtaBeginNak(const astros_packet_t &packet)
    {
        OtaBeginNakRecord rec;
        if (packet.packetType != AstrOsPacketType::OTA_BEGIN_NAK ||
            packet.payloadSize != static_cast<int>(sizeof(OtaBeginNakPayload)))
        {
            return rec;
        }
        const auto *p = reinterpret_cast<const OtaBeginNakPayload *>(packet.payload);
        if (p->reason > static_cast<uint8_t>(OtaBeginNakReason::BEGIN_FAILED))
        {
            return rec; // out-of-range reason
        }
        rec.xferId = p->xferId;
        rec.reason = static_cast<OtaBeginNakReason>(p->reason);
        rec.valid = true;
        return rec;
    }

    OtaDataRecord parseOtaData(const astros_packet_t &packet)
    {
        OtaDataRecord rec;
        if (packet.packetType != AstrOsPacketType::OTA_DATA ||
            packet.payloadSize < static_cast<int>(sizeof(OtaDataHeader)))
        {
            return rec;
        }
        const auto *hdr = reinterpret_cast<const OtaDataHeader *>(packet.payload);
        // The actual firmware-bytes count is packet.payloadSize - sizeof(header).
        // hdr->payloadLen MUST equal that, else reject.
        const int actualBytes = packet.payloadSize - static_cast<int>(sizeof(OtaDataHeader));
        if (static_cast<int>(hdr->payloadLen) != actualBytes)
        {
            return rec;
        }
        rec.xferId = hdr->xferId;
        rec.seq = hdr->seq;
        rec.payloadLen = hdr->payloadLen;
        rec.crc16 = hdr->crc16;
        rec.payload = packet.payload + sizeof(OtaDataHeader);
        rec.valid = true;
        return rec;
    }

    OtaDataAckRecord parseOtaDataAck(const astros_packet_t &packet)
    {
        OtaDataAckRecord rec;
        if (packet.packetType != AstrOsPacketType::OTA_DATA_ACK ||
            packet.payloadSize != static_cast<int>(sizeof(OtaDataAckPayload)))
        {
            return rec;
        }
        const auto *p = reinterpret_cast<const OtaDataAckPayload *>(packet.payload);
        rec.xferId = p->xferId;
        rec.highestContiguousSeq = p->highestContiguousSeq;
        rec.nextExpectedSeq = p->nextExpectedSeq;
        rec.windowRemaining = p->windowRemaining;
        rec.valid = true;
        return rec;
    }

    OtaDataNakRecord parseOtaDataNak(const astros_packet_t &packet)
    {
        OtaDataNakRecord rec;
        if (packet.packetType != AstrOsPacketType::OTA_DATA_NAK ||
            packet.payloadSize != static_cast<int>(sizeof(OtaDataNakPayload)))
        {
            return rec;
        }
        const auto *p = reinterpret_cast<const OtaDataNakPayload *>(packet.payload);
        if (p->reason < static_cast<uint8_t>(OtaDataNakReason::CRC) ||
            p->reason > static_cast<uint8_t>(OtaDataNakReason::WRITE))
        {
            return rec;
        }
        rec.xferId = p->xferId;
        rec.highestContiguousSeq = p->highestContiguousSeq;
        rec.nextExpectedSeq = p->nextExpectedSeq;
        rec.windowRemaining = p->windowRemaining;
        rec.reason = static_cast<OtaDataNakReason>(p->reason);
        rec.valid = true;
        return rec;
    }

    OtaEndRecord parseOtaEnd(const astros_packet_t &packet)
    {
        OtaEndRecord rec;
        if (packet.packetType != AstrOsPacketType::OTA_END ||
            packet.payloadSize != static_cast<int>(sizeof(OtaEndPayload)))
        {
            return rec;
        }
        const auto *p = reinterpret_cast<const OtaEndPayload *>(packet.payload);
        rec.xferId = p->xferId;
        rec.totalChunksSent = p->totalChunksSent;
        std::memcpy(rec.sha256Final, p->sha256Final, 32);
        rec.valid = true;
        return rec;
    }

    OtaEndAckRecord parseOtaEndAck(const astros_packet_t &packet)
    {
        OtaEndAckRecord rec;
        if (packet.packetType != AstrOsPacketType::OTA_END_ACK ||
            packet.payloadSize != static_cast<int>(sizeof(OtaEndAckPayload)))
        {
            return rec;
        }
        const auto *p = reinterpret_cast<const OtaEndAckPayload *>(packet.payload);
        if (p->status > static_cast<uint8_t>(OtaEndStatus::WRITE_ERROR))
        {
            return rec;
        }
        rec.xferId = p->xferId;
        rec.status = static_cast<OtaEndStatus>(p->status);
        std::memcpy(rec.sha256Computed, p->sha256Computed, 32);
        rec.valid = true;
        return rec;
    }

} // namespace AstrOsEspNowProtocol

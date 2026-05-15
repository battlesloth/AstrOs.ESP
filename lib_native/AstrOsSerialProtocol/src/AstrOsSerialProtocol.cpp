#include "AstrOsSerialProtocol.hpp"

#include <AstrOsStringUtils.hpp>

namespace AstrOsSerialProtocol
{
    namespace
    {
        constexpr const char *kBroadcastMac = "00:00:00:00:00:00";

        // Splits the validated payload group on RECORD_SEPARATOR to
        // produce one entry per controller record. The payload has
        // already been separated from the header by validateSerialMsg.
        std::vector<std::string> extractControllers(const std::string &payload)
        {
            if (payload.empty())
            {
                return {};
            }
            return AstrOsStringUtils::splitString(payload, RECORD_SEPARATOR);
        }

        void appendCommand(DecodeResult &result, AstrOsInterfaceResponseType responseType, const std::string &msgId,
                           const std::string &peerMac, const std::string &peerName, const std::string &message)
        {
            DecodedCommand cmd;
            cmd.responseType = responseType;
            cmd.msgId = msgId;
            cmd.peerMac = peerMac;
            cmd.peerName = peerName;
            cmd.message = message;
            result.commands.push_back(std::move(cmd));
        }

        void appendReject(DecodeResult &result, const std::string &entry, DecodeRejectReason reason)
        {
            DecodeReject rej;
            rej.entry = entry;
            rej.reason = reason;
            result.rejects.push_back(std::move(rej));
        }

        void decodeRegistrationSync(DecodeResult &result, const std::string &msgId)
        {
            // Historical quirk: handleRegistrationSync hardcoded
            // REGISTRATION_SYNC for both roles, bypassing mapResponseType
            // (which returns UNKNOWN for REG_SYNC on a padawan). Preserve
            // that bypass here so on-wire behaviour is bit-identical.
            appendCommand(result, AstrOsInterfaceResponseType::REGISTRATION_SYNC, msgId, "", "", "");
        }

        void decodeDeployConfig(DecodeResult &result, const std::string &msgId, const std::string &payload)
        {
            for (const auto &controller : extractControllers(payload))
            {
                auto msgParts = AstrOsStringUtils::splitString(controller, UNIT_SEPARATOR);
                if (msgParts.size() != 3)
                {
                    appendReject(result, controller, DecodeRejectReason::WRONG_PART_COUNT);
                    continue;
                }

                const bool broadcast = (msgParts[0] == kBroadcastMac);
                const auto responseType = mapResponseType(AstrOsSerialMessageType::DEPLOY_CONFIG, broadcast);
                const std::string peerMac = broadcast ? "" : msgParts[0];
                appendCommand(result, responseType, msgId, peerMac, "", msgParts[2]);
            }
        }

        void decodeDeployScript(DecodeResult &result, const std::string &msgId, const std::string &payload)
        {
            for (const auto &controller : extractControllers(payload))
            {
                auto msgParts = AstrOsStringUtils::splitString(controller, UNIT_SEPARATOR);
                if (msgParts.size() != 4)
                {
                    appendReject(result, controller, DecodeRejectReason::WRONG_PART_COUNT);
                    continue;
                }

                const std::string script = msgParts[2] + UNIT_SEPARATOR + msgParts[3];
                const bool broadcast = (msgParts[0] == kBroadcastMac);
                const auto responseType = mapResponseType(AstrOsSerialMessageType::DEPLOY_SCRIPT, broadcast);
                const std::string peerMac = broadcast ? "" : msgParts[0];
                appendCommand(result, responseType, msgId, peerMac, "", script);
            }
        }

        void decodeBasicCommand(DecodeResult &result, AstrOsSerialMessageType type, const std::string &msgId,
                                const std::string &payload)
        {
            for (const auto &controller : extractControllers(payload))
            {
                auto msgParts = AstrOsStringUtils::splitString(controller, UNIT_SEPARATOR);

                if (msgParts.size() != 3)
                {
                    appendReject(result, controller, DecodeRejectReason::WRONG_PART_COUNT);
                    continue;
                }
                if (msgParts[0].empty())
                {
                    appendReject(result, controller, DecodeRejectReason::EMPTY_DEST);
                    continue;
                }
                if (msgParts[2].empty())
                {
                    appendReject(result, controller, DecodeRejectReason::EMPTY_VALUE);
                    continue;
                }

                const bool broadcast = (msgParts[0] == kBroadcastMac);
                const auto responseType = mapResponseType(type, broadcast);
                const std::string peerMac = broadcast ? "" : msgParts[0];
                appendCommand(result, responseType, msgId, peerMac, "", msgParts[2]);
            }
        }

        void decodeFwInbound(DecodeResult &result, AstrOsSerialMessageType type, const std::string &msgId,
                             const std::string &payload)
        {
            // FW_* inbound payloads are not parsed here. A later MIXED
            // phase (the OTA receiver, not yet implemented) will own
            // structured parsing via parseFwTransferBegin / parseFwChunk
            // / parseFwTransferEnd / parseFwDeployBegin. For now we
            // route the raw payload through with the matching
            // responseType so the handler task can hand it to that
            // future component once it lands.
            const auto responseType = mapResponseType(type, /*isMaster=*/true);
            appendCommand(result, responseType, msgId, "", "", payload);
        }
    } // namespace

    AstrOsInterfaceResponseType mapResponseType(AstrOsSerialMessageType type, bool isMaster)
    {
        if (isMaster)
        {
            switch (type)
            {
            case AstrOsSerialMessageType::REGISTRATION_SYNC:
                return AstrOsInterfaceResponseType::REGISTRATION_SYNC;
            case AstrOsSerialMessageType::DEPLOY_CONFIG:
                return AstrOsInterfaceResponseType::SET_CONFIG;
            case AstrOsSerialMessageType::DEPLOY_SCRIPT:
                return AstrOsInterfaceResponseType::SAVE_SCRIPT;
            case AstrOsSerialMessageType::RUN_SCRIPT:
                return AstrOsInterfaceResponseType::SCRIPT_RUN;
            case AstrOsSerialMessageType::PANIC_STOP:
                return AstrOsInterfaceResponseType::PANIC_STOP;
            case AstrOsSerialMessageType::FORMAT_SD:
                return AstrOsInterfaceResponseType::FORMAT_SD;
            case AstrOsSerialMessageType::RUN_COMMAND:
                return AstrOsInterfaceResponseType::COMMAND;
            case AstrOsSerialMessageType::SERVO_TEST:
                return AstrOsInterfaceResponseType::SERVO_TEST;
            case AstrOsSerialMessageType::FW_TRANSFER_BEGIN:
                return AstrOsInterfaceResponseType::FW_TRANSFER_BEGIN;
            case AstrOsSerialMessageType::FW_CHUNK:
                return AstrOsInterfaceResponseType::FW_CHUNK;
            case AstrOsSerialMessageType::FW_TRANSFER_END:
                return AstrOsInterfaceResponseType::FW_TRANSFER_END;
            case AstrOsSerialMessageType::FW_DEPLOY_BEGIN:
                return AstrOsInterfaceResponseType::FW_DEPLOY_BEGIN;
            default:
                return AstrOsInterfaceResponseType::UNKNOWN;
            }
        }

        switch (type)
        {
        case AstrOsSerialMessageType::DEPLOY_CONFIG:
            return AstrOsInterfaceResponseType::SEND_CONFIG;
        case AstrOsSerialMessageType::DEPLOY_SCRIPT:
            return AstrOsInterfaceResponseType::SEND_SCRIPT;
        case AstrOsSerialMessageType::RUN_SCRIPT:
            return AstrOsInterfaceResponseType::SEND_SCRIPT_RUN;
        case AstrOsSerialMessageType::PANIC_STOP:
            return AstrOsInterfaceResponseType::SEND_PANIC_STOP;
        case AstrOsSerialMessageType::FORMAT_SD:
            return AstrOsInterfaceResponseType::SEND_FORMAT_SD;
        case AstrOsSerialMessageType::RUN_COMMAND:
            return AstrOsInterfaceResponseType::SEND_COMMAND;
        case AstrOsSerialMessageType::SERVO_TEST:
            return AstrOsInterfaceResponseType::SEND_SERVO_TEST;
        default:
            return AstrOsInterfaceResponseType::UNKNOWN;
        }
    }

    DecodeResult decodeSerialMessage(AstrOsSerialMessageType type, const std::string &msgId, const std::string &payload)
    {
        DecodeResult result;

        // Every type except REGISTRATION_SYNC expects a non-empty payload
        // group. Without one, the per-controller decoders would silently
        // produce zero commands and zero rejects — surfacing a reject here
        // keeps the boundary caller informed.
        if (payload.empty() && type != AstrOsSerialMessageType::REGISTRATION_SYNC &&
            type != AstrOsSerialMessageType::UNKNOWN)
        {
            appendReject(result, "", DecodeRejectReason::EMPTY_PAYLOAD);
            return result;
        }

        switch (type)
        {
        case AstrOsSerialMessageType::REGISTRATION_SYNC:
            decodeRegistrationSync(result, msgId);
            break;
        case AstrOsSerialMessageType::DEPLOY_CONFIG:
            decodeDeployConfig(result, msgId, payload);
            break;
        case AstrOsSerialMessageType::DEPLOY_SCRIPT:
            decodeDeployScript(result, msgId, payload);
            break;
        case AstrOsSerialMessageType::RUN_SCRIPT:
        case AstrOsSerialMessageType::PANIC_STOP:
        case AstrOsSerialMessageType::FORMAT_SD:
        case AstrOsSerialMessageType::RUN_COMMAND:
        case AstrOsSerialMessageType::SERVO_TEST:
            decodeBasicCommand(result, type, msgId, payload);
            break;
        case AstrOsSerialMessageType::FW_TRANSFER_BEGIN:
        case AstrOsSerialMessageType::FW_CHUNK:
        case AstrOsSerialMessageType::FW_TRANSFER_END:
        case AstrOsSerialMessageType::FW_DEPLOY_BEGIN:
            decodeFwInbound(result, type, msgId, payload);
            break;
        default:
            appendReject(result, payload, DecodeRejectReason::UNKNOWN_TYPE);
            break;
        }

        return result;
    }

    const char *describeRejectReason(DecodeRejectReason reason)
    {
        switch (reason)
        {
        case DecodeRejectReason::WRONG_PART_COUNT:
            return "wrong part count";
        case DecodeRejectReason::EMPTY_DEST:
            return "empty destination";
        case DecodeRejectReason::EMPTY_VALUE:
            return "empty value";
        case DecodeRejectReason::UNKNOWN_TYPE:
            return "unknown message type";
        case DecodeRejectReason::EMPTY_PAYLOAD:
            return "empty payload";
        }
        return "unknown";
    }
} // namespace AstrOsSerialProtocol

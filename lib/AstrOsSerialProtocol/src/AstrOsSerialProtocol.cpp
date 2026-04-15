#include "AstrOsSerialProtocol.hpp"

#include <AstrOsStringUtils.hpp>

namespace AstrOsSerialProtocol
{
    namespace
    {
        constexpr const char *kBroadcastMac = "00:00:00:00:00:00";

        // Pulls the payload group out of a validated serial message. The
        // wire format is `header GS payload`; `validateSerialMsg` has
        // already confirmed the header is well-formed, so parts[1] is the
        // payload group.
        std::vector<std::string> extractControllers(const std::string &message)
        {
            auto parts = AstrOsStringUtils::splitString(message, GROUP_SEPARATOR);
            if (parts.size() < 2)
            {
                return {};
            }
            return AstrOsStringUtils::splitString(parts[1], RECORD_SEPARATOR);
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

        void decodeDeployConfig(DecodeResult &result, const std::string &msgId, const std::string &message)
        {
            for (const auto &controller : extractControllers(message))
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

        void decodeDeployScript(DecodeResult &result, const std::string &msgId, const std::string &message)
        {
            for (const auto &controller : extractControllers(message))
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
                                const std::string &message)
        {
            for (const auto &controller : extractControllers(message))
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

    DecodeResult decodeSerialMessage(AstrOsSerialMessageType type, const std::string &msgId, const std::string &message,
                                     bool isMaster)
    {
        (void)isMaster; // retained for future per-role branching; DEPLOY_* and
                        // basic commands infer role from the per-controller mac.
        DecodeResult result;

        switch (type)
        {
        case AstrOsSerialMessageType::REGISTRATION_SYNC:
            decodeRegistrationSync(result, msgId);
            break;
        case AstrOsSerialMessageType::DEPLOY_CONFIG:
            decodeDeployConfig(result, msgId, message);
            break;
        case AstrOsSerialMessageType::DEPLOY_SCRIPT:
            decodeDeployScript(result, msgId, message);
            break;
        case AstrOsSerialMessageType::RUN_SCRIPT:
        case AstrOsSerialMessageType::PANIC_STOP:
        case AstrOsSerialMessageType::FORMAT_SD:
        case AstrOsSerialMessageType::RUN_COMMAND:
        case AstrOsSerialMessageType::SERVO_TEST:
            decodeBasicCommand(result, type, msgId, message);
            break;
        default:
            appendReject(result, message, DecodeRejectReason::UNKNOWN_TYPE);
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
        }
        return "unknown";
    }
} // namespace AstrOsSerialProtocol

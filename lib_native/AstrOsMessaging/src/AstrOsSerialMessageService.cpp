#include "AstrOsSerialMessageService.hpp"
#include <AstrOsStringUtils.hpp>

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

AstrOsSerialMessageService::AstrOsSerialMessageService()
{
    this->msgTypeMap = {
        {AstrOsSerialMessageType::REGISTRATION_SYNC, AstrOsSC::REGISTRATION_SYNC},
        {AstrOsSerialMessageType::REGISTRATION_SYNC_ACK, AstrOsSC::REGISTRATION_SYNC_ACK},
        {AstrOsSerialMessageType::DEPLOY_CONFIG, AstrOsSC::DEPLOY_CONFIG},
        {AstrOsSerialMessageType::DEPLOY_CONFIG_ACK, AstrOsSC::DEPLOY_CONFIG_ACK},
        {AstrOsSerialMessageType::DEPLOY_CONFIG_NAK, AstrOsSC::DEPLOY_CONFIG_NAK},
        {AstrOsSerialMessageType::POLL_ACK, AstrOsSC::POLL_ACK},
        {AstrOsSerialMessageType::POLL_NAK, AstrOsSC::POLL_NAK},
        {AstrOsSerialMessageType::DEPLOY_SCRIPT, AstrOsSC::DEPLOY_SCRIPT},
        {AstrOsSerialMessageType::DEPLOY_SCRIPT_ACK, AstrOsSC::DEPLOY_SCRIPT_ACK},
        {AstrOsSerialMessageType::DEPLOY_SCRIPT_NAK, AstrOsSC::DEPLOY_SCRIPT_NAK},
        {AstrOsSerialMessageType::RUN_SCRIPT, AstrOsSC::RUN_SCRIPT},
        {AstrOsSerialMessageType::RUN_SCRIPT_ACK, AstrOsSC::RUN_SCRIPT_ACK},
        {AstrOsSerialMessageType::RUN_SCRIPT_NAK, AstrOsSC::RUN_SCRIPT_NAK},
        {AstrOsSerialMessageType::RUN_COMMAND, AstrOsSC::RUN_COMMAND},
        {AstrOsSerialMessageType::RUN_COMMAND_ACK, AstrOsSC::RUN_COMMAND_ACK},
        {AstrOsSerialMessageType::RUN_COMMAND_NAK, AstrOsSC::RUN_COMMAND_NAK},
        {AstrOsSerialMessageType::FORMAT_SD, AstrOsSC::FORMAT_SD},
        {AstrOsSerialMessageType::FORMAT_SD_ACK, AstrOsSC::FORMAT_SD_ACK},
        {AstrOsSerialMessageType::FORMAT_SD_NAK, AstrOsSC::FORMAT_SD_NAK},
        {AstrOsSerialMessageType::PANIC_STOP, AstrOsSC::PANIC_STOP},
        {AstrOsSerialMessageType::SERVO_TEST, AstrOsSC::SERVO_TEST},
        {AstrOsSerialMessageType::SERVO_TEST_ACK, AstrOsSC::SERVO_TEST_ACK},
        {AstrOsSerialMessageType::FW_TRANSFER_BEGIN, AstrOsSC::FW_TRANSFER_BEGIN},
        {AstrOsSerialMessageType::FW_TRANSFER_BEGIN_ACK, AstrOsSC::FW_TRANSFER_BEGIN_ACK},
        {AstrOsSerialMessageType::FW_CHUNK, AstrOsSC::FW_CHUNK},
        {AstrOsSerialMessageType::FW_CHUNK_ACK, AstrOsSC::FW_CHUNK_ACK},
        {AstrOsSerialMessageType::FW_CHUNK_NAK, AstrOsSC::FW_CHUNK_NAK},
        {AstrOsSerialMessageType::FW_TRANSFER_END, AstrOsSC::FW_TRANSFER_END},
        {AstrOsSerialMessageType::FW_TRANSFER_END_ACK, AstrOsSC::FW_TRANSFER_END_ACK},
        {AstrOsSerialMessageType::FW_DEPLOY_BEGIN, AstrOsSC::FW_DEPLOY_BEGIN},
        {AstrOsSerialMessageType::FW_PROGRESS, AstrOsSC::FW_PROGRESS},
        {AstrOsSerialMessageType::FW_DEPLOY_DONE, AstrOsSC::FW_DEPLOY_DONE},
        {AstrOsSerialMessageType::FW_BACKPRESSURE, AstrOsSC::FW_BACKPRESSURE},
    };
}

AstrOsSerialMessageService::~AstrOsSerialMessageService() {}

/// @brief validates serial messages by comparing the header to the expected values. Header consists of 2 parts, type
/// enum and validation string
/// @param header serial message header
/// @return is valid
astros_serial_msg_validation_t AstrOsSerialMessageService::validateSerialMsg(std::string msg)
{
    astros_serial_msg_validation_t result{"", "", AstrOsSerialMessageType::UNKNOWN, false};

    auto msgParts = AstrOsStringUtils::splitString(msg, GROUP_SEPARATOR);

    auto parts = AstrOsStringUtils::splitString(msgParts[0], RECORD_SEPARATOR);

    if (parts.size() != 3)
    {
        return result;
    }

    auto type = static_cast<AstrOsSerialMessageType>(stoi(parts[0]));

    if (this->msgTypeMap.find(type) == this->msgTypeMap.end())
    {
        return result;
    }

    auto validator = this->msgTypeMap[type];

    if (memcmp(parts[1].c_str(), validator.c_str(), validator.size()) != 0)
    {
        return result;
    }

    result.valid = true;
    result.msgId = parts[2];
    result.payload = msgParts.size() > 1 ? msgParts[1] : "";
    result.type = type;

    return result;
}

/// @brief generates a serial message header from the type and validation string
/// @param type AstrOsSerialMessageType
/// @param validation string
/// @return header string
std::string AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType type, std::string msgId)
{
    if (this->msgTypeMap.find(type) == this->msgTypeMap.end())
    {
        return "";
    }

    auto validation = this->msgTypeMap[type];

    std::stringstream ss;
    ss << std::to_string(static_cast<int>(type)) << RECORD_SEPARATOR << validation << RECORD_SEPARATOR << msgId
       << GROUP_SEPARATOR;
    return ss.str();
}

/// @brief generates a registration sync acknowledgment message which contains a list of registered controlellers
/// @param peers list of registered controllers
/// @return serial message
std::string AstrOsSerialMessageService::getRegistrationSyncAck(std::string msgId,
                                                               std::vector<astros_peer_data_t> controllers)
{
    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType::REGISTRATION_SYNC_ACK, msgId);

    for (const auto &p : controllers)
    {
        ss << p.mac << UNIT_SEPARATOR << p.name << UNIT_SEPARATOR << p.fingerprint << RECORD_SEPARATOR;
    }

    std::string message = ss.str();

    message.pop_back();

    return message;
}

/// @brief generates a poll acknowledgment message which contains the name, fingerprint, and firmware version
///        of the controller identified by macAddress. Caller is responsible for passing the *correct* version
///        for that mac — when forwarding a peer's POLL_ACK, this must be the peer's version, not the local one.
/// @param macAddress peer mac address
/// @param controller peer controller name
/// @param fingerprint peer configuration fingerprint
/// @param firmwareVersion peer firmware version (may be empty for legacy peers; server treats empty as incompatible)
/// @return serial message
std::string AstrOsSerialMessageService::getPollAck(std::string macAddress, std::string controller,
                                                   std::string fingerprint, std::string firmwareVersion)
{
    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType::POLL_ACK, "na");
    ss << macAddress << UNIT_SEPARATOR << controller << UNIT_SEPARATOR << fingerprint << UNIT_SEPARATOR
       << firmwareVersion;
    return ss.str();
}

/// @brief generates a poll failure message which contains the name of the peer that did not repond in time to the poll
/// @param macAddress mac address of the peer
/// @param name peer controller
/// @return serial message
std::string AstrOsSerialMessageService::getPollNak(std::string macAddress, std::string controller)
{
    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType::POLL_NAK, "na");
    ss << macAddress << UNIT_SEPARATOR << controller;
    return ss.str();
}

/// @brief generates a basic acknowledgment or failure message
/// @param type AstrOsSerialMessageType
/// @param msgId message id
/// @param controller peer controller
/// @param id message id
/// @return serial message
std::string AstrOsSerialMessageService::getBasicAckNak(AstrOsSerialMessageType type, std::string msgId,
                                                       std::string macAddress, std::string controller, std::string data)
{
    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(type, msgId);
    ss << macAddress << UNIT_SEPARATOR << controller << UNIT_SEPARATOR << data;
    return ss.str();
}

/// @brief generates FW_TRANSFER_BEGIN_ACK reply. Payload shape per .docs/protocol.md:
///        transfer-id<US>status where status is "OK" or a snake_case rejection code
///        (sd_full, busy, unsupported_version, io_error).
/// @param msgId echo of the BEGIN's msgId
/// @param transferId transfer id assigned by the server (server's choice; opaque to us)
/// @param status "OK" on success, otherwise a snake_case rejection code
/// @return serial message
std::string AstrOsSerialMessageService::getFwTransferBeginAck(std::string msgId, std::string transferId,
                                                              std::string status)
{
    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType::FW_TRANSFER_BEGIN_ACK, msgId);
    ss << transferId << UNIT_SEPARATOR << status;
    return ss.str();
}

/// @brief generates FW_CHUNK_ACK reply. Payload shape:
///        transfer-id<US>highest-contiguous-seq<US>next-expected-seq<US>window-remaining.
/// @param transferId transfer id
/// @param highestContiguousSeq highest seq we've committed in order
/// @param nextExpectedSeq the seq we want next (always highestContiguousSeq + 1 in our impl)
/// @param windowRemaining how many more in-flight frames the sender may have
/// @return serial message
std::string AstrOsSerialMessageService::getFwChunkAck(std::string transferId, uint32_t highestContiguousSeq,
                                                      uint32_t nextExpectedSeq, uint8_t windowRemaining)
{
    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType::FW_CHUNK_ACK, "na");
    ss << transferId << UNIT_SEPARATOR << std::to_string(highestContiguousSeq) << UNIT_SEPARATOR
       << std::to_string(nextExpectedSeq) << UNIT_SEPARATOR << std::to_string(static_cast<unsigned>(windowRemaining));
    return ss.str();
}

/// @brief generates FW_CHUNK_NAK reply. Payload shape:
///        transfer-id<US>last-good-seq<US>reason-code.
/// @param transferId transfer id
/// @param lastGoodSeq the last seq we committed (server resumes from N+1)
/// @param reasonCode "CRC" | "SIZE" | "OUT_OF_ORDER" | "FLASH_FULL"
/// @return serial message
std::string AstrOsSerialMessageService::getFwChunkNak(std::string transferId, uint32_t lastGoodSeq,
                                                      std::string reasonCode)
{
    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType::FW_CHUNK_NAK, "na");
    ss << transferId << UNIT_SEPARATOR << std::to_string(lastGoodSeq) << UNIT_SEPARATOR << reasonCode;
    return ss.str();
}

/// @brief generates FW_TRANSFER_END_ACK reply. Payload shape:
///        transfer-id<US>status<US>computed-sha256-hex where status is
///        OK | HASH_MISMATCH | IO_ERROR (SCREAMING per .docs/protocol.md).
/// @param msgId echo of the END's msgId
/// @param transferId transfer id
/// @param status OK | HASH_MISMATCH | IO_ERROR
/// @param computedSha256Hex 64 lowercase hex chars of master's computed hash
/// @return serial message
std::string AstrOsSerialMessageService::getFwTransferEndAck(std::string msgId, std::string transferId,
                                                            std::string status, std::string computedSha256Hex)
{
    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType::FW_TRANSFER_END_ACK, msgId);
    ss << transferId << UNIT_SEPARATOR << status << UNIT_SEPARATOR << computedSha256Hex;
    return ss.str();
}

/// @brief generates FW_DEPLOY_DONE. Payload shape per .docs/protocol.md:
///        transfer-id<US>per-controller-result-list, RS-separated results
///        of controllerId<US>OK|FAILED<US>finalVersion<US>errorOrEmpty.
///        Transfer-id is prepended once; it precedes the first result inline,
///        which therefore has 5 US-separated fields on the wire while subsequent
///        results have 4.
/// @param msgId echo of the originating msgId (the DEPLOY_BEGIN that triggered this)
/// @param transferId transfer id
/// @param results per-target result records
/// @return serial message
std::string AstrOsSerialMessageService::getFwDeployDone(std::string msgId, std::string transferId,
                                                        std::vector<astros_fw_deploy_result_t> results)
{
    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType::FW_DEPLOY_DONE, msgId);
    ss << transferId;
    for (size_t i = 0; i < results.size(); i++)
    {
        const auto &r = results[i];
        if (i == 0)
        {
            // First result is inline with transferId, connected by US (5 US-sep fields total on this RS-record)
            ss << UNIT_SEPARATOR << r.controllerId << UNIT_SEPARATOR << r.status << UNIT_SEPARATOR << r.finalVersion
               << UNIT_SEPARATOR << r.errorOrEmpty;
        }
        else
        {
            // Subsequent results are RS-separated, 4 US-sep fields per record
            ss << RECORD_SEPARATOR << r.controllerId << UNIT_SEPARATOR << r.status << UNIT_SEPARATOR << r.finalVersion
               << UNIT_SEPARATOR << r.errorOrEmpty;
        }
    }
    return ss.str();
}

//================== TEST METHODS ==================

/// @brief FOR TESTING PURPOSES. generates a registration sync command message
/// @param msgId message id
/// @return serial message
std::string AstrOsSerialMessageService::getRegistrationSync(std::string msgId)
{
    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType::REGISTRATION_SYNC, msgId);
    return ss.str();
}

/// @brief FOR TESTING PURPOSES. generates a deploy config message
/// @param msgId message id
/// @param macs list of mac addresses to deploy the config to
/// @param controllers list of controllers to deploy the config to
/// @param configs list of configs to deploy, indexed to controllers list
/// @return serial message
std::string AstrOsSerialMessageService::getDeployConfig(std::string msgId, std::vector<std::string> macs,
                                                        std::vector<std::string> controllers,
                                                        std::vector<std::string> configs)
{
    if (controllers.size() != configs.size())
    {
        return "";
    }

    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType::DEPLOY_CONFIG, msgId);

    for (size_t i = 0; i < controllers.size(); i++)
    {
        ss << macs[i] << UNIT_SEPARATOR << controllers[i] << UNIT_SEPARATOR << "32" << UNIT_SEPARATOR << configs[i]
           << RECORD_SEPARATOR;
    }

    std::string message = ss.str();
    message.pop_back();

    return message;
}

/// @brief FOR TESTING PURPOSES. generates a deploy script message
/// @param msgId message id
/// @param scriptId script id
/// @param controllers list of controllers to deploy the script to
/// @param scripts list of scripts to deploy, indexed to controllers list
/// @return serial message
std::string AstrOsSerialMessageService::getDeployScript(std::string msgId, std::string scriptId,
                                                        std::vector<std::string> controllers,
                                                        std::vector<std::string> scripts)
{
    if (controllers.size() != scripts.size())
    {
        return "";
    }

    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType::DEPLOY_SCRIPT, msgId);

    for (size_t i = 0; i < controllers.size(); i++)
    {
        ss << controllers[i] << UNIT_SEPARATOR << scripts[i] << RECORD_SEPARATOR;
    }

    std::string message = ss.str();
    message.pop_back();

    return message;
}

/// @brief FOR TESTING PURPOSES. generates a run script message
/// @param msgId message id
/// @param scriptId script id
/// @return serial message
std::string AstrOsSerialMessageService::getRunScript(std::string msgId, std::string scriptId)
{
    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType::RUN_SCRIPT, msgId);
    ss << GROUP_SEPARATOR << scriptId;
    return ss.str();
}

/// @brief FOR TESTING PURPOSES. generates a run command message
/// @param msgId message id
/// @param controller controller to run the command on
/// @param command command to run
/// @return serial message
std::string AstrOsSerialMessageService::getRunCommand(std::string msgId, std::string controller, std::string command)
{
    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType::RUN_COMMAND, msgId);
    ss << GROUP_SEPARATOR << command;
    return ss.str();
}

/// @brief FOR TESTING PURPOSES. generates a panic stop message
/// @param msgId message id
/// @return serial message
std::string AstrOsSerialMessageService::getPanicStop(std::string msgId)
{
    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType::PANIC_STOP, msgId);
    return ss.str();
}

/// @brief FOR TESTING PURPOSES. generates a format sd message
/// @param msgId message id
/// @return serial message
std::string AstrOsSerialMessageService::getFormatSD(std::string msgId)
{
    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType::FORMAT_SD, msgId);
    return ss.str();
}

/// @brief FOR TESTING PURPOSES. generates a servo test message
/// @param msgId message id
/// @param macAddress mac address of the peer
/// @param controller controller to run the command on
/// @param data data to send
/// @return serial message
std::string AstrOsSerialMessageService::getServoTest(std::string msgId, std::string macAddress, std::string controller,
                                                     std::string data)
{
    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType::SERVO_TEST, msgId);
    ss << macAddress << UNIT_SEPARATOR << controller << UNIT_SEPARATOR << data;
    return ss.str();
}

//================== FREE PARSERS FOR INBOUND FW_* PAYLOADS ==================

FwTransferBeginRecord parseFwTransferBegin(const std::string &payload)
{
    FwTransferBeginRecord rec{};
    rec.valid = false;

    auto parts = AstrOsStringUtils::splitString(payload, UNIT_SEPARATOR);
    if (parts.size() != 5)
    {
        return rec;
    }

    // size + chunk-size: parse with strtoul, catch parse failure via errno.
    // The codebase avoids exceptions on the embedded target (CLAUDE.md), so
    // use strtoul which signals errors via errno + endptr.
    errno = 0;
    char *endptr = nullptr;
    auto totalSize = std::strtoul(parts[1].c_str(), &endptr, 10);
    if (errno != 0 || endptr == parts[1].c_str() || *endptr != '\0')
    {
        return rec;
    }
    errno = 0;
    endptr = nullptr;
    auto chunkSize = std::strtoul(parts[3].c_str(), &endptr, 10);
    if (errno != 0 || endptr == parts[3].c_str() || *endptr != '\0' || chunkSize > 0xFFFFu)
    {
        return rec;
    }

    // sha256-hex must be exactly 64 chars
    if (parts[2].size() != 64)
    {
        return rec;
    }

    // target-list: RS-split. Reject if empty (no targets).
    auto targets = AstrOsStringUtils::splitString(parts[4], RECORD_SEPARATOR);
    if (targets.empty() || (targets.size() == 1 && targets[0].empty()))
    {
        return rec;
    }

    rec.transferId = parts[0];
    rec.totalSize = static_cast<uint32_t>(totalSize);
    rec.sha256Hex = parts[2];
    rec.chunkSize = static_cast<uint16_t>(chunkSize);
    rec.targetIds = std::move(targets);
    rec.valid = true;
    return rec;
}

namespace
{
    // Parses exactly 4 hex chars (lowercase or uppercase) into a uint16_t.
    // Returns false on length mismatch or non-hex character.
    bool parseHex16(const std::string &hex, uint16_t &out)
    {
        if (hex.size() != 4)
        {
            return false;
        }
        uint16_t v = 0;
        for (char c : hex)
        {
            uint8_t nibble = 0;
            if (c >= '0' && c <= '9')
                nibble = c - '0';
            else if (c >= 'a' && c <= 'f')
                nibble = 10 + (c - 'a');
            else if (c >= 'A' && c <= 'F')
                nibble = 10 + (c - 'A');
            else
                return false;
            v = static_cast<uint16_t>((v << 4) | nibble);
        }
        out = v;
        return true;
    }
} // namespace

FwChunkRecord parseFwChunk(const std::string &payload)
{
    FwChunkRecord rec{};
    rec.valid = false;

    auto parts = AstrOsStringUtils::splitString(payload, UNIT_SEPARATOR);
    if (parts.size() != 5)
    {
        return rec;
    }

    errno = 0;
    char *endptr = nullptr;
    auto seq = std::strtoul(parts[1].c_str(), &endptr, 10);
    if (errno != 0 || endptr == parts[1].c_str() || *endptr != '\0')
    {
        return rec;
    }
    errno = 0;
    endptr = nullptr;
    auto plen = std::strtoul(parts[2].c_str(), &endptr, 10);
    if (errno != 0 || endptr == parts[2].c_str() || *endptr != '\0' || plen > 0xFFFFu)
    {
        return rec;
    }

    uint16_t crc = 0;
    if (!parseHex16(parts[4], crc))
    {
        return rec;
    }

    rec.transferId = parts[0];
    rec.seq = static_cast<uint32_t>(seq);
    rec.payloadLen = static_cast<uint16_t>(plen);
    rec.base64Payload = parts[3];
    rec.crc16 = crc;
    rec.valid = true;
    return rec;
}

FwTransferEndRecord parseFwTransferEnd(const std::string &payload)
{
    FwTransferEndRecord rec{};
    rec.valid = false;

    auto parts = AstrOsStringUtils::splitString(payload, UNIT_SEPARATOR);
    if (parts.size() != 3)
    {
        return rec;
    }

    errno = 0;
    char *endptr = nullptr;
    auto totalChunks = std::strtoul(parts[1].c_str(), &endptr, 10);
    if (errno != 0 || endptr == parts[1].c_str() || *endptr != '\0')
    {
        return rec;
    }

    if (parts[2].size() != 64)
    {
        return rec;
    }

    rec.transferId = parts[0];
    rec.totalChunks = static_cast<uint32_t>(totalChunks);
    rec.finalSha256Hex = parts[2];
    rec.valid = true;
    return rec;
}

FwDeployBeginRecord parseFwDeployBegin(const std::string &payload)
{
    FwDeployBeginRecord rec{};
    rec.valid = false;

    auto parts = AstrOsStringUtils::splitString(payload, UNIT_SEPARATOR);
    if (parts.size() != 2)
    {
        return rec;
    }

    auto orderIds = AstrOsStringUtils::splitString(parts[1], RECORD_SEPARATOR);
    if (orderIds.empty() || (orderIds.size() == 1 && orderIds[0].empty()))
    {
        return rec;
    }

    rec.transferId = parts[0];
    rec.orderIds = std::move(orderIds);
    rec.valid = true;
    return rec;
}

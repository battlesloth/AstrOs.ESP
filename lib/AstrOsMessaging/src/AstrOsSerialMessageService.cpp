#include "AstrOsSerialMessageService.hpp"
#include "AstrOsMessageUtil.hpp"
#include <AstrOsStringUtils.hpp>

#include <cmath>
#include <string>
#include <sstream>
#include <cstring>
#include <cstdint>

AstrOsSerialMessageService::AstrOsSerialMessageService()
{
}

AstrOsSerialMessageService::~AstrOsSerialMessageService()
{
}

/// @brief validates serial messages by comparing the header to the expected values. Header consists of 2 parts, type enum and validation string
/// @param header serial message header
/// @return is valid
astros_serial_msg_validation_t AstrOsSerialMessageService::validateSerialMsg(std::string msg)
{

    astros_serial_msg_validation_t result{"", AstrOsSerialMessageType::UNKNOWN, false};

    auto msgParts = AstrOsStringUtils::splitString(msg, GROUP_SEPARATOR);

    auto parts = AstrOsStringUtils::splitString(msgParts[0], RECORD_SEPARATOR);

    if (parts.size() != 3)
    {
        return result;
    }

    auto type = static_cast<AstrOsSerialMessageType>(stoi(parts[0]));

    switch (type)
    {
    case AstrOsSerialMessageType::REGISTRATION_SYNC:
        result.valid = memcmp(parts[1].c_str(), AstrOsSC::REGISTRATION_SYNC, strlen(AstrOsSC::REGISTRATION_SYNC)) == 0;
        break;
    case AstrOsSerialMessageType::REGISTRATION_SYNC_ACK:
        result.valid = memcmp(parts[1].c_str(), AstrOsSC::REGISTRATION_SYNC_ACK, strlen(AstrOsSC::REGISTRATION_SYNC_ACK)) == 0;
        break;
    case AstrOsSerialMessageType::DEPLOY_CONFIG:
        result.valid = memcmp(parts[1].c_str(), AstrOsSC::DEPLOY_CONFIG, strlen(AstrOsSC::DEPLOY_CONFIG)) == 0;
        break;
    case AstrOsSerialMessageType::DEPLOY_CONFIG_ACK:
        result.valid = memcmp(parts[1].c_str(), AstrOsSC::DEPLOY_CONFIG_ACK, strlen(AstrOsSC::DEPLOY_CONFIG_ACK)) == 0;
        break;
    case AstrOsSerialMessageType::DEPLOY_CONFIG_NAK:
        result.valid = memcmp(parts[1].c_str(), AstrOsSC::DEPLOY_CONFIG_NAK, strlen(AstrOsSC::DEPLOY_CONFIG_NAK)) == 0;
        break;
    case AstrOsSerialMessageType::POLL_ACK:
        result.valid = memcmp(parts[1].c_str(), AstrOsSC::POLL_ACK, strlen(AstrOsSC::POLL_ACK)) == 0;
        break;
    case AstrOsSerialMessageType::POLL_NAK:
        result.valid = memcmp(parts[1].c_str(), AstrOsSC::POLL_NAK, strlen(AstrOsSC::POLL_NAK)) == 0;
        break;
    case AstrOsSerialMessageType::DEPLOY_SCRIPT:
        result.valid = memcmp(parts[1].c_str(), AstrOsSC::DEPLOY_SCRIPT, strlen(AstrOsSC::DEPLOY_SCRIPT)) == 0;
        break;
    case AstrOsSerialMessageType::DEPLOY_SCRIPT_ACK:
        result.valid = memcmp(parts[1].c_str(), AstrOsSC::DEPLOY_SCRIPT_ACK, strlen(AstrOsSC::DEPLOY_SCRIPT_ACK)) == 0;
        break;
    case AstrOsSerialMessageType::DEPLOY_SCRIPT_NAK:
        result.valid = memcmp(parts[1].c_str(), AstrOsSC::DEPLOY_SCRIPT_NAK, strlen(AstrOsSC::DEPLOY_SCRIPT_NAK)) == 0;
        break;
    case AstrOsSerialMessageType::RUN_SCRIPT:
        result.valid = memcmp(parts[1].c_str(), AstrOsSC::RUN_SCRIPT, strlen(AstrOsSC::RUN_SCRIPT)) == 0;
        break;
    case AstrOsSerialMessageType::RUN_SCRIPT_ACK:
        result.valid = memcmp(parts[1].c_str(), AstrOsSC::RUN_SCRIPT_ACK, strlen(AstrOsSC::RUN_SCRIPT_ACK)) == 0;
        break;
    case AstrOsSerialMessageType::RUN_SCRIPT_NAK:
        result.valid = memcmp(parts[1].c_str(), AstrOsSC::RUN_SCRIPT_NAK, strlen(AstrOsSC::RUN_SCRIPT_NAK)) == 0;
        break;
    case AstrOsSerialMessageType::RUN_COMMAND:
        result.valid = memcmp(parts[1].c_str(), AstrOsSC::RUN_COMMAND, strlen(AstrOsSC::RUN_COMMAND)) == 0;
        break;
    case AstrOsSerialMessageType::RUN_COMMAND_ACK:
        result.valid = memcmp(parts[1].c_str(), AstrOsSC::RUN_COMMAND_ACK, strlen(AstrOsSC::RUN_COMMAND_ACK)) == 0;
        break;
    case AstrOsSerialMessageType::RUN_COMMAND_NAK:
        result.valid = memcmp(parts[1].c_str(), AstrOsSC::RUN_COMMAND_NAK, strlen(AstrOsSC::RUN_COMMAND_NAK)) == 0;
        break;
    default:
        result.valid = false;
        return result;
    }

    if (result.valid)
    {
        result.msgId = parts[2];
        result.type = type;
    }

    return result;
}

/// @brief generates a serial message header from the type and validation string
/// @param type AstrOsSerialMessageType
/// @param validation string
/// @return header string
std::string AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType type, std::string msgId)
{
    auto validation = AstrOsSerialMessageService::getValidationString(type);

    std::stringstream ss;
    ss << std::to_string(static_cast<int>(type)) << RECORD_SEPARATOR << validation << RECORD_SEPARATOR << msgId << GROUP_SEPARATOR;
    return ss.str();
}

/// @brief gets the validation string for a given message type
/// @param type AstrOsSerialMessageType
/// @return char* validation string
const char *AstrOsSerialMessageService::getValidationString(AstrOsSerialMessageType type)
{
    switch (type)
    {
    case AstrOsSerialMessageType::REGISTRATION_SYNC:
        return AstrOsSC::REGISTRATION_SYNC;
    case AstrOsSerialMessageType::REGISTRATION_SYNC_ACK:
        return AstrOsSC::REGISTRATION_SYNC_ACK;
    case AstrOsSerialMessageType::DEPLOY_CONFIG:
        return AstrOsSC::DEPLOY_CONFIG;
    case AstrOsSerialMessageType::DEPLOY_CONFIG_ACK:
        return AstrOsSC::DEPLOY_CONFIG_ACK;
    case AstrOsSerialMessageType::DEPLOY_CONFIG_NAK:
        return AstrOsSC::DEPLOY_CONFIG_NAK;
    case AstrOsSerialMessageType::POLL_ACK:
        return AstrOsSC::POLL_ACK;
    case AstrOsSerialMessageType::POLL_NAK:
        return AstrOsSC::POLL_NAK;
    case AstrOsSerialMessageType::DEPLOY_SCRIPT:
        return AstrOsSC::DEPLOY_SCRIPT;
    case AstrOsSerialMessageType::DEPLOY_SCRIPT_ACK:
        return AstrOsSC::DEPLOY_SCRIPT_ACK;
    case AstrOsSerialMessageType::DEPLOY_SCRIPT_NAK:
        return AstrOsSC::DEPLOY_SCRIPT_NAK;
    case AstrOsSerialMessageType::RUN_SCRIPT:
        return AstrOsSC::RUN_SCRIPT;
    case AstrOsSerialMessageType::RUN_SCRIPT_ACK:
        return AstrOsSC::RUN_SCRIPT_ACK;
    case AstrOsSerialMessageType::RUN_SCRIPT_NAK:
        return AstrOsSC::RUN_SCRIPT_NAK;
    case AstrOsSerialMessageType::RUN_COMMAND:
        return AstrOsSC::RUN_COMMAND;
    case AstrOsSerialMessageType::RUN_COMMAND_ACK:
        return AstrOsSC::RUN_COMMAND_ACK;
    case AstrOsSerialMessageType::RUN_COMMAND_NAK:
        return AstrOsSC::RUN_COMMAND_NAK;
    default:
        return "";
    }
}

/// @brief generates a registration sync acknowledgment message which contains a list of registered controlellers
/// @param peers list of registered controllers
/// @return serial message
std::string AstrOsSerialMessageService::getRegistrationSyncAck(std::string msgId, std::vector<astros_peer_data_t> controllers)
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

/// @brief generates a poll acknowledgment message which contains the name and fingerprint of the peer
/// @param macAddress peer mac address
/// @param name peer controller
/// @param fingerprint configuration fingerprint
/// @return serial message
std::string AstrOsSerialMessageService::getPollAck(std::string macAddress, std::string controller, std::string fingerprint)
{
    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType::POLL_ACK, "na");
    ss << macAddress << UNIT_SEPARATOR << controller << UNIT_SEPARATOR << fingerprint;
    return ss.str();
}

/// @brief generates a poll failure message which contains the name of the peer that did not repond in time to the poll
/// @param macAddress mac address of the peer
/// @param name peer controller
/// @return serial message
std::string AstrOsSerialMessageService::getPollNak(std::string macAddress, char *controller)
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
std::string AstrOsSerialMessageService::getBasicAckNak(AstrOsSerialMessageType type, std::string msgId, std::string macAddress, std::string controller, std::string data)
{
    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(type, msgId);
    ss << macAddress << UNIT_SEPARATOR << controller << UNIT_SEPARATOR << data;
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
std::string AstrOsSerialMessageService::getDeployConfig(std::string msgId, std::vector<std::string> macs, std::vector<std::string> controllers, std::vector<std::string> configs)
{
    if (controllers.size() != configs.size())
    {
        return "";
    }

    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType::DEPLOY_CONFIG, msgId);

    for (size_t i = 0; i < controllers.size(); i++)
    {
        ss << macs[i] << UNIT_SEPARATOR << controllers[i] << UNIT_SEPARATOR << "32" << UNIT_SEPARATOR << configs[i] << RECORD_SEPARATOR;
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
std::string AstrOsSerialMessageService::getDeployScript(std::string msgId, std::string scriptId, std::vector<std::string> controllers, std::vector<std::string> scripts)
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
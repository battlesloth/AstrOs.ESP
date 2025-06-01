#include "AstrOsSerialMessageService.hpp"
#include <AstrOsStringUtils.hpp>

#include <cmath>
#include <string>
#include <sstream>
#include <cstring>
#include <cstdint>

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
    };
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
    ss << std::to_string(static_cast<int>(type)) << RECORD_SEPARATOR << validation << RECORD_SEPARATOR << msgId << GROUP_SEPARATOR;
    return ss.str();
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
std::string AstrOsSerialMessageService::getServoTest(std::string msgId, std::string macAddress, std::string controller, std::string data)
{
    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType::SERVO_TEST, msgId);
    ss << macAddress << UNIT_SEPARATOR << controller << UNIT_SEPARATOR << data;
    return ss.str();
}
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

std::string AstrOsSerialMessageService::generateRegistrationSyncMsg(std::vector<astros_peer_data_t> peers)
{
    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType::REGISTRATION_SYNC, AstrOsSC::REGISTRATION_SYNC);
    ss << GROUP_SEPARATOR;

    for (const auto &p : peers)
    {
        ss << p.name << UNIT_SEPARATOR << p.mac << RECORD_SEPARATOR;
    }

    std::string message = ss.str();

    message.pop_back();

    return message;
}

std::string AstrOsSerialMessageService::generatePollAckMsg(std::string name, std::string fingerprint)
{
    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType::POLL_ACK, AstrOsSC::POLL_ACK);
    ss << GROUP_SEPARATOR << name << UNIT_SEPARATOR << fingerprint;
    return ss.str();
}

std::string AstrOsSerialMessageService::generatePollNakMsg(char *name)
{
    std::stringstream ss;
    ss << AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType::POLL_NAK, AstrOsSC::POLL_NAK);
    ss << GROUP_SEPARATOR << name;
    return ss.str();
}

std::string AstrOsSerialMessageService::generateHeader(AstrOsSerialMessageType type, std::string validation)
{
    std::stringstream ss;
    ss << std::to_string(static_cast<int>(type)) << RECORD_SEPARATOR << validation;
    return ss.str();
}

bool AstrOsSerialMessageService::validateSerialMsg(std::string header)
{
    auto parts = AstrOsStringUtils::splitString(header, RECORD_SEPARATOR);

    if (parts.size() != 2)
    {
        return false;
    }

    switch (static_cast<AstrOsSerialMessageType>(stoi(parts[0])))
    {
    case AstrOsSerialMessageType::REGISTRATION_SYNC:
        return memcmp(parts[2].c_str(), AstrOsSC::REGISTRATION_SYNC, strlen(AstrOsSC::REGISTRATION_SYNC)) == 0;
    case AstrOsSerialMessageType::POLL_ACK:
        return memcmp(parts[2].c_str(), AstrOsSC::POLL_ACK, strlen(AstrOsSC::POLL_ACK)) == 0;
    case AstrOsSerialMessageType::POLL_NAK:
        return memcmp(parts[2].c_str(), AstrOsSC::POLL_NAK, strlen(AstrOsSC::POLL_NAK)) == 0;
    default:
        return false;
    }

    return false;
}

#include "AstrOsSerialMessageService.hpp"
#include "AstrOsMessageUtil.hpp"

#include <cmath>
#include <string>
#include <cstring>
#include <cstdint>

AstrOsSerialMessageService::AstrOsSerialMessageService()
{
}

AstrOsSerialMessageService::~AstrOsSerialMessageService()
{
}

astros_serial_message_t AstrOsSerialMessageService::generateHeartBeatMsg(std::string name)
{
    std::string message = std::string(AstrOsSC::HEARTBEAT) + UNIT_SEPARATOR + name;
    return AstrOsSerialMessageService::generateSerialMsg(AstrOsSerialMessageType::HEARTBEAT, message);
}

astros_serial_message_t AstrOsSerialMessageService::generateRegistrationSyncMsg(std::vector<astros_peer_data_t> peers)
{
    std::string message = std::string(AstrOsSC::REGISTRATION_SYNC) + UNIT_SEPARATOR;

    for (const auto &p : peers)
    {
        message += p.name + ':' + p.mac + UNIT_SEPARATOR;
    }

    message.pop_back();

    return AstrOsSerialMessageService::generateSerialMsg(AstrOsSerialMessageType::REGISTRATION_SYNC, message);
}

astros_serial_message_t AstrOsSerialMessageService::generateSerialMsg(AstrOsSerialMessageType type, std::string message)
{
    astros_serial_message_t msg;

    msg.messageType = type;
    msg.messageSize = 3 + message.length();
    msg.message = (uint8_t *)malloc(msg.messageSize);

    int payloadSize = message.length();
    int offset = 0;
    memcpy(msg.message, &type, 1);
    offset += 1;
    memcpy(msg.message + offset, &payloadSize, 2);
    offset += 2;
    memcpy(msg.message + offset, message.c_str(), message.length());
    offset += message.length();

    return msg;
}

astros_serial_message_t AstrOsSerialMessageService::parseSerialMsg(uint8_t *data)
{
    astros_serial_message_t msg;
    msg.messageType = (AstrOsSerialMessageType)data[0];
    int payloadSize = (data[2] << 8) | data[1];
    msg.messageSize = payloadSize;
    msg.message = (uint8_t *)malloc(payloadSize);
    memcpy(msg.message, data + SERIAL_MESSAGE_HEADER_SIZE, payloadSize);

    if (!validateSerialMsg(msg))
    {
        msg.messageType = AstrOsSerialMessageType::UNKNOWN;
    }

    return msg;
}

bool AstrOsSerialMessageService::validateSerialMsg(astros_serial_message_t msg)
{

    switch (msg.messageType)
    {
    case AstrOsSerialMessageType::HEARTBEAT:
        return memcmp(msg.message, AstrOsSC::HEARTBEAT, strlen(AstrOsSC::HEARTBEAT)) == 0;
    case AstrOsSerialMessageType::REGISTRATION_SYNC:
        return memcmp(msg.message, AstrOsSC::REGISTRATION_SYNC, strlen(AstrOsSC::REGISTRATION_SYNC)) == 0;
    default:
        return false;
    }

    return false;
}

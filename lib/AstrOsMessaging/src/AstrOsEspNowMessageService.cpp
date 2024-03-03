#include "AstrOsEspNowMessageService.hpp"
#include "AstrOsMessageUtil.hpp"

#include <cmath>
#include <string>
#include <cstring>
#include <cstdint>

std::vector<astros_espnow_data_t> AstrOsEspNowMessageService::generateEspNowMsg(AstrOsPacketType type, std::string mac, std::string message)
{
    switch (type)
    {
    case AstrOsPacketType::REGISTRATION_REQ:
        return AstrOsEspNowMessageService::generatePackets(type, AstrOsENC::REGISTRATION_REQ, "");
        break;
    case AstrOsPacketType::REGISTRATION:
        return AstrOsEspNowMessageService::generatePackets(type, std::string(AstrOsENC::REGISTRATION), mac + UNIT_SEPARATOR + message);
        break;
    case AstrOsPacketType::REGISTRATION_ACK:
        return AstrOsEspNowMessageService::generatePackets(type, std::string(AstrOsENC::REGISTRATION_ACK), mac + UNIT_SEPARATOR + message);
        break;
    case AstrOsPacketType::POLL:
        return AstrOsEspNowMessageService::generatePackets(type, std::string(AstrOsENC::POLL), mac);
        break;
    case AstrOsPacketType::POLL_ACK:
        return AstrOsEspNowMessageService::generatePackets(type, std::string(AstrOsENC::POLL_ACK), mac + UNIT_SEPARATOR + message);
        break;
    case AstrOsPacketType::CONFIG:
        return AstrOsEspNowMessageService::generatePackets(type, std::string(AstrOsENC::CONFIG), mac + UNIT_SEPARATOR + message);
        break;
    default:
        return AstrOsEspNowMessageService::generatePackets(type, "", message);
        break;
    }

    return std::vector<astros_espnow_data_t>();
}

std::vector<astros_espnow_data_t> AstrOsEspNowMessageService::generatePackets(AstrOsPacketType type, std::string validator, std::string message)
{
    std::vector<astros_espnow_data_t> packets;

    int messageLength = message.length();
    int totalPackets = 1;

    // account for adding validator to each payload
    int usablePayloadSize = ASTROS_PACKET_PAYLOAD_SIZE - (validator.size() + 1);

    if (messageLength != 0)
    {
        totalPackets = ceil((float)messageLength / (float)(usablePayloadSize));
    }

    int packetNumber = 0;
    int actualPayloadSize = 0;
    int payloadSize = 0;

    std::string payload;

    uint8_t *id = AstrOsEspNowMessageService::generateId();

    for (int i = 0; i < totalPackets; i++)
    {
        packetNumber = i + 1;

        payloadSize = usablePayloadSize;
        actualPayloadSize = ASTROS_PACKET_PAYLOAD_SIZE;
        if (i == totalPackets - 1)
        {
            payloadSize = messageLength - (i * usablePayloadSize);
            actualPayloadSize = payloadSize + validator.size() + 1;
        }
        payload = validator + UNIT_SEPARATOR + message.substr(i * usablePayloadSize, payloadSize);

        uint8_t *packet = (uint8_t *)malloc(20 + actualPayloadSize);

        int offset = 0;
        memcpy(packet, id, 16);
        offset += 16;
        memcpy(packet + offset, &packetNumber, 1);
        offset += 1;
        memcpy(packet + offset, &totalPackets, 1);
        offset += 1;
        memcpy(packet + offset, &type, 1);
        offset += 1;
        memcpy(packet + offset, &actualPayloadSize, 1);
        offset += 1;
        memcpy(packet + offset, payload.c_str(), actualPayloadSize);

        astros_espnow_data_t data = {packet, static_cast<size_t>(actualPayloadSize + 20)};

        packets.push_back(data);
    }

    free(id);

    return packets;
}

astros_packet_t AstrOsEspNowMessageService::parsePacket(uint8_t *packet)
{
    astros_packet_t parsedPacket;
    memcpy(parsedPacket.id, packet, 16);
    parsedPacket.packetNumber = packet[16];
    parsedPacket.totalPackets = packet[17];
    parsedPacket.packetType = (AstrOsPacketType)packet[18];
    parsedPacket.payloadSize = packet[19];
    parsedPacket.payload = packet + 20;

    auto validated = validatePacket(parsedPacket);
    if (validated == -1)
    {
        parsedPacket.packetType = AstrOsPacketType::UNKNOWN;
    }
    else // remove validator from payload
    {
        parsedPacket.payloadSize = ((int)packet[19]) - validated;
        parsedPacket.payload = packet + 20 + validated;
    }

    return parsedPacket;
}

/// @brief validates that the packet payload contains the expected validator and returns the number of bytes to remove from the payload
/// to remove validator, a -1 indicates the packet is invalid
/// @param packet
/// @return number of bytes to remove from the payload
int AstrOsEspNowMessageService::validatePacket(astros_packet_t packet)
{

    int validatorLength = -1;

    switch (packet.packetType)
    {
    case AstrOsPacketType::BASIC:
        if ((memcmp(packet.payload, AstrOsENC::BASIC, strlen(AstrOsENC::BASIC)) == 0))
        {
            validatorLength = strlen(AstrOsENC::BASIC) + 1;
        }
        break;
    case AstrOsPacketType::REGISTRATION_REQ:
        if ((memcmp(packet.payload, AstrOsENC::REGISTRATION_REQ, strlen(AstrOsENC::REGISTRATION_REQ)) == 0))
        {
            validatorLength = strlen(AstrOsENC::REGISTRATION_REQ) + 1;
        }
        break;
    case AstrOsPacketType::REGISTRATION:
        if ((memcmp(packet.payload, AstrOsENC::REGISTRATION, strlen(AstrOsENC::REGISTRATION)) == 0))
        {
            validatorLength = strlen(AstrOsENC::REGISTRATION) + 1;
        }
        break;
    case AstrOsPacketType::REGISTRATION_ACK:
        if ((memcmp(packet.payload, AstrOsENC::REGISTRATION_ACK, strlen(AstrOsENC::REGISTRATION_ACK)) == 0))
        {
            validatorLength = strlen(AstrOsENC::REGISTRATION_ACK) + 1;
        }
        break;
    case AstrOsPacketType::POLL:
        if ((memcmp(packet.payload, AstrOsENC::POLL, strlen(AstrOsENC::POLL)) == 0))
        {
            validatorLength = strlen(AstrOsENC::POLL) + 1;
        }
        break;
    case AstrOsPacketType::POLL_ACK:
        if ((memcmp(packet.payload, AstrOsENC::POLL_ACK, strlen(AstrOsENC::POLL_ACK)) == 0))
        {
            validatorLength = strlen(AstrOsENC::POLL_ACK) + 1;
        }
        break;
    case AstrOsPacketType::CONFIG:
        if ((memcmp(packet.payload, AstrOsENC::CONFIG, strlen(AstrOsENC::CONFIG)) == 0))
        {
            validatorLength = strlen(AstrOsENC::CONFIG) + 1;
        }
        break;
    case AstrOsPacketType::CONFIG_ACK:
        if ((memcmp(packet.payload, AstrOsENC::CONFIG_ACK, strlen(AstrOsENC::CONFIG_ACK)) == 0))
        {
            validatorLength = strlen(AstrOsENC::CONFIG_ACK) + 1;
        }
        break;
    case AstrOsPacketType::CONFIG_NAK:
        if ((memcmp(packet.payload, AstrOsENC::CONFIG_NAK, strlen(AstrOsENC::CONFIG_NAK)) == 0))
        {
            validatorLength = strlen(AstrOsENC::CONFIG_NAK) + 1;
        }
        break;
    default:
        break;
    }

    return validatorLength;
}

uint8_t *AstrOsEspNowMessageService::generateId()
{
    uint8_t *id = (uint8_t *)malloc(16);
    for (int i = 0; i < 16; i++)
    {
        id[i] = rand() % 256;
    }
    return id;
}
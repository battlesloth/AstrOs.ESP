#include "AstrOsEspNowMessageService.hpp"
#include <AstrOsStringUtils.hpp>

#include <cmath>
#include <string>
#include <cstring>
#include <cstdint>

AstrOsEspNowMessageService::AstrOsEspNowMessageService()
{
    packetTypeMap[AstrOsPacketType::UNKNOWN] = AstrOsENC::UNKNOWN;
    packetTypeMap[AstrOsPacketType::BASIC] = AstrOsENC::BASIC;
    packetTypeMap[AstrOsPacketType::REGISTRATION_REQ] = AstrOsENC::REGISTRATION_REQ;
    packetTypeMap[AstrOsPacketType::REGISTRATION] = AstrOsENC::REGISTRATION;
    packetTypeMap[AstrOsPacketType::REGISTRATION_ACK] = AstrOsENC::REGISTRATION_ACK;
    packetTypeMap[AstrOsPacketType::POLL] = AstrOsENC::POLL;
    packetTypeMap[AstrOsPacketType::POLL_ACK] = AstrOsENC::POLL_ACK;
    packetTypeMap[AstrOsPacketType::CONFIG] = AstrOsENC::CONFIG;
    packetTypeMap[AstrOsPacketType::CONFIG_ACK] = AstrOsENC::CONFIG_ACK;
    packetTypeMap[AstrOsPacketType::CONFIG_NAK] = AstrOsENC::CONFIG_NAK;
    packetTypeMap[AstrOsPacketType::SCRIPT_DEPLOY] = AstrOsENC::SCRIPT_DEPLOY;
    packetTypeMap[AstrOsPacketType::SCRIPT_DEPLOY_ACK] = AstrOsENC::SCRIPT_DEPLOY_ACK;
    packetTypeMap[AstrOsPacketType::SCRIPT_DEPLOY_NAK] = AstrOsENC::SCRIPT_DEPLOY_NAK;
    packetTypeMap[AstrOsPacketType::SCRIPT_RUN] = AstrOsENC::SCRIPT_RUN;
    packetTypeMap[AstrOsPacketType::SCRIPT_RUN_ACK] = AstrOsENC::SCRIPT_RUN_ACK;
    packetTypeMap[AstrOsPacketType::SCRIPT_RUN_NAK] = AstrOsENC::SCRIPT_RUN_NAK;
    packetTypeMap[AstrOsPacketType::PANIC_STOP] = AstrOsENC::PANIC_STOP;
    packetTypeMap[AstrOsPacketType::FORMAT_SD] = AstrOsENC::FORMAT_SD;
    packetTypeMap[AstrOsPacketType::FORMAT_SD_ACK] = AstrOsENC::FORMAT_SD_ACK;
    packetTypeMap[AstrOsPacketType::FORMAT_SD_NAK] = AstrOsENC::FORMAT_SD_NAK;
    packetTypeMap[AstrOsPacketType::SERVO_TEST] = AstrOsENC::SERVO_TEST;
    packetTypeMap[AstrOsPacketType::SERVO_TEST_ACK] = AstrOsENC::SERVO_TEST_ACK;
}

AstrOsEspNowMessageService::~AstrOsEspNowMessageService()
{
}

std::vector<astros_espnow_data_t> AstrOsEspNowMessageService::generateEspNowMsg(AstrOsPacketType type, std::string mac, std::string message)
{
    if (this->packetTypeMap.find(type) == this->packetTypeMap.end())
    {
        return std::vector<astros_espnow_data_t>();
    }

    auto msg = mac.size() > 0 && message.size() > 0 ? mac + UNIT_SEPARATOR + message : mac;

    return AstrOsEspNowMessageService::generatePackets(type, msg);
}

std::vector<astros_espnow_data_t> AstrOsEspNowMessageService::generatePackets(AstrOsPacketType type, std::string message)
{
    std::vector<astros_espnow_data_t> packets;

    auto validator = this->packetTypeMap[type];

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
    auto validator = this->packetTypeMap[packet.packetType];

    if (memcmp(packet.payload, validator.c_str(), validator.size()) != 0)
    {
        return -1;
    }
    else
    {
        return validator.size() + 1;
    }
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
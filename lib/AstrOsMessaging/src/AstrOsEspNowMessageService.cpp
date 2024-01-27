#include "AstrOsEspNowMessageService.h"
#include <cmath>
#include <string>
#include <cstring>
#include <cstdint>

uint8_t *AstrOsEspNowMessageService::generateEspNowMsg(AstrOsPacketType type, std::string name, std::string message)
{
    switch (type)
    {
    case AstrOsPacketType::BASIC:
        break;
    case AstrOsPacketType::REGISTRATION:
        return AstrOsEspNowMessageService::generatePackets(type, "REGISTRATION|" + name)[0];
        break;
    case AstrOsPacketType::REGISTRATION_ACK:
        return AstrOsEspNowMessageService::generatePackets(type, "REGISTRATION_ACK|" + name)[0];
        break;
    case AstrOsPacketType::HEARTBEAT:
        return AstrOsEspNowMessageService::generatePackets(type, "HEARTBEAT|" + name)[0];
        break;
    default:
        return AstrOsEspNowMessageService::generatePackets(type, message)[0];
        break;
    }

    return NULL;
}

std::vector<uint8_t *> AstrOsEspNowMessageService::generatePackets(AstrOsPacketType type, std::string message)
{
    std::vector<uint8_t *> packets;

    int messageLength = message.length();
    int totalPackets = ceil((float)messageLength / (float)ASTROS_PACKET_PAYLOAD_SIZE);

    int packetNumber = 0;
    int payloadSize = 0;
    std::string payload;

    uint8_t *id = AstrOsEspNowMessageService::generateId();

    for (int i = 0; i < totalPackets; i++)
    {
        packetNumber = i + 1;
        payloadSize = ASTROS_PACKET_PAYLOAD_SIZE;
        if (i == totalPackets - 1)
        {
            payloadSize = messageLength - (i * ASTROS_PACKET_PAYLOAD_SIZE);
        }
        payload = message.substr(i * ASTROS_PACKET_PAYLOAD_SIZE, payloadSize);

        uint8_t *packet = (uint8_t *)malloc(20 + payloadSize);

        int offset = 0;
        memcpy(packet, id, 16);
        offset += 16;
        memcpy(packet + offset, &packetNumber, 1);
        offset += 1;
        memcpy(packet + offset, &totalPackets, 1);
        offset += 1;
        memcpy(packet + offset, &type, 1);
        offset += 1;
        memcpy(packet + offset, &payloadSize, 1);
        offset += 1;
        memcpy(packet + offset, payload.c_str(), payloadSize);

        packets.push_back(packet);
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

    return parsedPacket;
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
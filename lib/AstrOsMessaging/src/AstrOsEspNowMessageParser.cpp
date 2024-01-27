#include "AstrOsEspNowMessageParser.h"
#include <cmath>
#include <string>
#include <cstring>
#include <cstdint>

std::vector<uint8_t *> AstrOsEspNowMessageParser::generatePackets(AstrOsPacketType type, std::string message)
{
    std::vector<uint8_t *> packets;

    int messageLength = message.length();
    int totalPackets = ceil((float)messageLength / (float)ASTROS_PACKET_PAYLOAD_SIZE);

    int packetNumber = 0;
    int payloadSize = 0;
    std::string payload;

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
        memcpy(packet, AstrOsEspNowMessageParser::generateId(), 16);
        offset += 16;
        memcpy(packet, &packetNumber + offset, 1);
        offset += 1;
        memcpy(packet, &totalPackets + offset, 1);
        offset += 1;
        memcpy(packet, &type + offset, 1);
        offset += 1;
        memcpy(packet, &payloadSize + offset, 1);
        offset += 1;
        memcpy(packet, payload.c_str() + offset, payloadSize);

        packets.push_back(packet);
    }

    return packets;
}

astros_packet_t AstrOsEspNowMessageParser::parsePacket(uint8_t *packet)
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

uint8_t *AstrOsEspNowMessageParser::generateId()
{
    uint8_t *id = (uint8_t *)malloc(16);
    for (int i = 0; i < 16; i++)
    {
        id[i] = rand() % 256;
    }
    return id;
}
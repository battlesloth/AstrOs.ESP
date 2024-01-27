#ifndef ASTROSESPNOWMESSAGEPARSER_H
#define ASTROSESPNOWMESSAGEPARSER_H

#include <string>
#include <vector>
#include <cstdint>

#define ASTROS_PACKET_PAYLOAD_SIZE 180

// |----ID-----|-number-|--of---|-type--|-payload size-|---payload---|
// | uint8[16] | uint8  | uint8 | uint8 |    uint8     |  uint8[180] |
typedef enum
{
    REGISTRATION,
    HEARTBEAT,
} AstrOsPacketType;

typedef struct
{
    uint8_t id[16];
    int packetNumber;
    int totalPackets;
    AstrOsPacketType packetType;
    int payloadSize;
    uint8_t *payload;
} astros_packet_t;

class AstrOsEspNowMessageParser
{

private:
    static uint8_t *generateId();

public:
    AstrOsEspNowMessageParser();
    ~AstrOsEspNowMessageParser();

    static std::vector<uint8_t *> generatePackets(AstrOsPacketType type, std::string message);
    static astros_packet_t parsePacket(uint8_t *packet);
};

#endif

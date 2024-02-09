#ifndef ASTROSESPNOWMESSAGESERVICE_H
#define ASTROSESPNOWMESSAGESERVICE_H

#include <string>
#include <vector>
#include <cstdint>

#define ASTROS_PACKET_PAYLOAD_SIZE 180

// Packet definition
// |----ID-----|-number-|--of---|-type--|-payload size-|---payload---|
// | uint8[16] | uint8  | uint8 | uint8 |    uint8     |  uint8[180] |

// ENC - ESP-NOW Contansts
namespace AstrOsENC
{
    constexpr const static char *UNKNOWN = "UNKNOWN";
    constexpr const static char *BASIC = "BASIC";
    constexpr const static char *REGISTRATION_REQ = "REGISTRATION_REQ";
    constexpr const static char *REGISTRATION = "REGISTRATION";
    constexpr const static char *REGISTRATION_ACK = "REGISTRATION_ACK";
    constexpr const static char *POLL = "POLL";
    constexpr const static char *POLL_ACK = "POLL_ACK";
}

enum class AstrOsPacketType
{
    UNKNOWN,
    BASIC,
    REGISTRATION_REQ,
    REGISTRATION,
    REGISTRATION_ACK,
    POLL,
    POLL_ACK
};

typedef struct
{
    uint8_t id[16];
    int packetNumber;
    int totalPackets;
    AstrOsPacketType packetType;
    int payloadSize;
    uint8_t *payload;
} astros_packet_t;

typedef struct
{
    uint8_t *data;
    size_t size;
} astros_espnow_data_t;

class AstrOsEspNowMessageService
{

private:
    static uint8_t *generateId();

public:
    AstrOsEspNowMessageService();
    ~AstrOsEspNowMessageService();

    static astros_espnow_data_t generateEspNowMsg(AstrOsPacketType type, std::string name = "", std::string message = "");
    static std::vector<astros_espnow_data_t> generatePackets(AstrOsPacketType type, std::string message);
    static astros_packet_t parsePacket(uint8_t *packet);
    static bool validatePacket(astros_packet_t packet);
};

#endif

#ifndef ASTROSESPNOWMESSAGESERVICE_H
#define ASTROSESPNOWMESSAGESERVICE_H

#include <string>
#include <vector>
#include <map>
#include <cstdint>

#define ASTROS_PACKET_PAYLOAD_SIZE 180

// Packet definition
// |----ID-----|-number-|--of---|-type--|-payload size-|---payload---|
// | uint8[16] | uint8  | uint8 | uint8 |    uint8     |  uint8[180] |

// payload definition
//  |---validator---|US|---message---|

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
    constexpr const static char *CONFIG = "CONFIG";
    constexpr const static char *CONFIG_ACK = "CONFIG_ACK";
    constexpr const static char *CONFIG_NAK = "CONFIG_NAK";
    constexpr const static char *SCRIPT_DEPLOY = "SCRIPT_DEPLOY";
    constexpr const static char *SCRIPT_DEPLOY_ACK = "SCRIPT_DEPLOY_ACK";
    constexpr const static char *SCRIPT_DEPLOY_NAK = "SCRIPT_DEPLOY_NAK";
    constexpr const static char *SCRIPT_RUN = "SCRIPT_RUN";
    constexpr const static char *SCRIPT_RUN_ACK = "RUN_SCRIPT_ACK";
    constexpr const static char *SCRIPT_RUN_NAK = "RUN_SCRIPT_NAK";
    constexpr const static char *PANIC_STOP = "PANIC_STOP";
    constexpr const static char *FORMAT_SD = "FORMAT_SD";
    constexpr const static char *FORMAT_SD_ACK = "FORMAT_SD_ACK";
    constexpr const static char *FORMAT_SD_NAK = "FORMAT_SD_NAK";
}

enum class AstrOsPacketType
{
    UNKNOWN,
    BASIC,
    REGISTRATION_REQ,
    REGISTRATION,
    REGISTRATION_ACK,
    POLL,
    POLL_ACK,
    CONFIG,
    CONFIG_ACK,
    CONFIG_NAK,
    SCRIPT_DEPLOY,
    SCRIPT_DEPLOY_ACK,
    SCRIPT_DEPLOY_NAK,
    SCRIPT_RUN,
    SCRIPT_RUN_ACK,
    SCRIPT_RUN_NAK,
    COMMAND_RUN,
    COMMAND_RUN_ACK,
    COMMAND_RUN_NAK,
    PANIC_STOP,
    FORMAT_SD,
    FORMAT_SD_ACK,
    FORMAT_SD_NAK
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
    uint8_t *generateId();
    std::map<AstrOsPacketType, std::string> packetTypeMap;

public:
    AstrOsEspNowMessageService();
    ~AstrOsEspNowMessageService();

    std::vector<astros_espnow_data_t> generateEspNowMsg(AstrOsPacketType type, std::string mac = "", std::string message = "");
    std::vector<astros_espnow_data_t> generatePackets(AstrOsPacketType type, std::string message);
    astros_packet_t parsePacket(uint8_t *packet);
    int validatePacket(astros_packet_t packet);
};

#endif

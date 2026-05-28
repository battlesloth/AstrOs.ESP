#ifndef ASTROSESPNOWMESSAGESERVICE_H
#define ASTROSESPNOWMESSAGESERVICE_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

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
    constexpr const static char *COMMAND_RUN = "COMMAND_RUN";
    constexpr const static char *COMMAND_RUN_ACK = "COMMAND_RUN_ACK";
    constexpr const static char *COMMAND_RUN_NAK = "COMMAND_RUN_NAK";
    constexpr const static char *PANIC_STOP = "PANIC_STOP";
    constexpr const static char *FORMAT_SD = "FORMAT_SD";
    constexpr const static char *FORMAT_SD_ACK = "FORMAT_SD_ACK";
    constexpr const static char *FORMAT_SD_NAK = "FORMAT_SD_NAK";
    constexpr const static char *SERVO_TEST = "SERVO_TEST";
    constexpr const static char *SERVO_TEST_ACK = "SERVO_TEST_ACK";
    constexpr const static char *OTA_BEGIN = "OTA_BEGIN";
    constexpr const static char *OTA_BEGIN_ACK = "OTA_BEGIN_ACK";
    constexpr const static char *OTA_BEGIN_NAK = "OTA_BEGIN_NAK";
    constexpr const static char *OTA_DATA = "OTA_DATA";
    constexpr const static char *OTA_DATA_ACK = "OTA_DATA_ACK";
    constexpr const static char *OTA_DATA_NAK = "OTA_DATA_NAK";
    constexpr const static char *OTA_END = "OTA_END";
    constexpr const static char *OTA_END_ACK = "OTA_END_ACK";
    constexpr const static char *OTA_FLASH_RESULT = "OTA_FLASH_RESULT";
} // namespace AstrOsENC

// Wire-stable: NEVER renumber existing variants. Always append new variants at the end with the next sequential value.
// Underlying type is uint8_t — the wire format encodes this enum as a single byte (see generatePackets and
// parsePacket).
enum class AstrOsPacketType : uint8_t
{
    UNKNOWN = 0,
    BASIC = 1,
    REGISTRATION_REQ = 2,
    REGISTRATION = 3,
    REGISTRATION_ACK = 4,
    POLL = 5,
    POLL_ACK = 6,
    CONFIG = 7,
    CONFIG_ACK = 8,
    CONFIG_NAK = 9,
    SCRIPT_DEPLOY = 10,
    SCRIPT_DEPLOY_ACK = 11,
    SCRIPT_DEPLOY_NAK = 12,
    SCRIPT_RUN = 13,
    SCRIPT_RUN_ACK = 14,
    SCRIPT_RUN_NAK = 15,
    COMMAND_RUN = 16,
    COMMAND_RUN_ACK = 17,
    COMMAND_RUN_NAK = 18,
    PANIC_STOP = 19,
    FORMAT_SD = 20,
    FORMAT_SD_ACK = 21,
    FORMAT_SD_NAK = 22,
    SERVO_TEST = 23,
    SERVO_TEST_ACK = 24,
    OTA_BEGIN = 25,
    OTA_BEGIN_ACK = 26,
    OTA_BEGIN_NAK = 27,
    OTA_DATA = 28,
    OTA_DATA_ACK = 29,
    OTA_DATA_NAK = 30,
    OTA_END = 31,
    OTA_END_ACK = 32,
    OTA_FLASH_RESULT = 33, // padawan → master flash-commit outcome
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

    std::vector<astros_espnow_data_t> generateEspNowMsg(AstrOsPacketType type, std::string mac = "",
                                                        std::string message = "");
    std::vector<astros_espnow_data_t> generatePackets(AstrOsPacketType type, std::string message);
    // Binary-frame builder for OTA packets. Unlike generateEspNowMsg/generatePackets,
    // this path does NOT inject a validator-string prefix into the payload — the full
    // ASTROS_PACKET_PAYLOAD_SIZE budget is available for binary content. Always
    // produces exactly one packet (OTA frames fit in a single ESP-NOW transmission
    // by design). Returns an empty vector if `type` is not an OTA type or `len`
    // exceeds ASTROS_PACKET_PAYLOAD_SIZE.
    std::vector<astros_espnow_data_t> generateOtaPacket(AstrOsPacketType type, const uint8_t *payload, size_t len);
    astros_packet_t parsePacket(uint8_t *packet);
    int validatePacket(astros_packet_t packet);
};

// True iff `type` is one of the OTA_* packet types.
bool isOtaPacketType(AstrOsPacketType type);

#endif

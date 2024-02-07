#ifndef ASTROSSERIALMESSAGESERVICE_H
#define ASTROSSERIALMESSAGESERVICE_H

#include <string>
#include <vector>
#include <cstdint>

#define SERIAL_MESSAGE_HEADER_SIZE 3

//|--type--|-payload size-|-------payload--------|
//|  uint8 |   uint8[2]   |  uint8[payload size] |

// Serial Contansts
namespace AstrOsSC
{
    constexpr const static char *HEARTBEAT = "HEARTBEAT";
    constexpr const static char *REGISTRATION_SYNC = "REGISTRATION_SYNC";
}

enum class AstrOsSerialMessageType
{
    UNKNOWN,
    HEARTBEAT,
    REGISTRATION_SYNC
};

typedef struct
{
    AstrOsSerialMessageType messageType;
    int messageSize;
    uint8_t *message;
} astros_serial_message_t;

typedef struct
{
    std::string name;
    std::string mac;
} astros_peer_data_t;

class AstrOsSerialMessageService
{
private:
    static bool validateSerialMsg(astros_serial_message_t msg);
    static astros_serial_message_t generateSerialMsg(AstrOsSerialMessageType type, std::string message);

public:
    AstrOsSerialMessageService();
    ~AstrOsSerialMessageService();

    static astros_serial_message_t parseSerialMsg(uint8_t *data);
    static astros_serial_message_t generateHeartBeatMsg(std::string name);
    static astros_serial_message_t generateRegistrationSyncMsg(std::vector<astros_peer_data_t> peers);
};

#endif
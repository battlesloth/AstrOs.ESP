#ifndef ASTROSSERIALMESSAGESERVICE_H
#define ASTROSSERIALMESSAGESERVICE_H

#include <string>
#include <vector>
#include <cstdint>

#define SERIAL_MESSAGE_HEADER_SIZE 3

//|--type--| -validation--|------------payload------------|
//|--int---US---string---GS--val 1--US--val 2--US--val N--|

// Serial Contansts
namespace AstrOsSC
{
    constexpr const static char *REGISTRATION_SYNC = "REGISTRATION_SYNC";
    constexpr const static char *POLL_ACK = "POLL_ACK";
    constexpr const static char *POLL_NAK = "POLL_NAK";
}

enum class AstrOsSerialMessageType
{
    UNKNOWN,
    REGISTRATION_SYNC,
    POLL_ACK,
    POLL_NAK
};

typedef struct
{
    std::string name;
    std::string mac;
} astros_peer_data_t;

class AstrOsSerialMessageService
{
private:
    static bool validateSerialMsg(std::string msg);
    static std::string generateHeader(AstrOsSerialMessageType type, std::string validation);

public:
    AstrOsSerialMessageService();
    ~AstrOsSerialMessageService();

    static bool handleSerialMsg(uint8_t *data);
    static std::string generateRegistrationSyncMsg(std::vector<astros_peer_data_t> peers);
    static std::string generatePollAckMsg(std::string name, std::string fingerprint);
    static std::string generatePollNakMsg(char *name);
};

#endif
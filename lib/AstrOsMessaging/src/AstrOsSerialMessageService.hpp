#ifndef ASTROSSERIALMESSAGESERVICE_H
#define ASTROSSERIALMESSAGESERVICE_H

#include <string>
#include <map>
#include <vector>
#include <cstdint>

#define SERIAL_MESSAGE_HEADER_SIZE 3

//|--type--|--validation--|---msg Id---|---------------payload-------------|
//|--int---RS---string----RS--string---GS--val--US--val--RS--val--US--val--|

// Serial Contansts
namespace AstrOsSC
{
    constexpr const static char *REGISTRATION_SYNC = "REGISTRATION_SYNC";
    constexpr const static char *REGISTRATION_SYNC_ACK = "REGISTRATION_SYNC_ACK";
    constexpr const static char *POLL_ACK = "POLL_ACK";
    constexpr const static char *POLL_NAK = "POLL_NAK";
    constexpr const static char *DEPLOY_CONFIG = "DEPLOY_CONFIG";
    constexpr const static char *DEPLOY_CONFIG_ACK = "DEPLOY_CONFIG_ACK";
    constexpr const static char *DEPLOY_CONFIG_NAK = "DEPLOY_CONFIG_NAK";
    constexpr const static char *DEPLOY_SCRIPT = "DEPLOY_SCRIPT";
    constexpr const static char *DEPLOY_SCRIPT_ACK = "DEPLOY_SCRIPT_ACK";
    constexpr const static char *DEPLOY_SCRIPT_NAK = "DEPLOY_SCRIPT_NAK";
    constexpr const static char *RUN_SCRIPT = "RUN_SCRIPT";
    constexpr const static char *RUN_SCRIPT_ACK = "RUN_SCRIPT_ACK";
    constexpr const static char *RUN_SCRIPT_NAK = "RUN_SCRIPT_NAK";
    constexpr const static char *PANIC_STOP = "PANIC_STOP";
    constexpr const static char *RUN_COMMAND = "RUN_COMMAND";
    constexpr const static char *RUN_COMMAND_ACK = "RUN_COMMAND_ACK";
    constexpr const static char *RUN_COMMAND_NAK = "RUN_COMMAND_NAK";
    constexpr const static char *FORMAT_SD = "FORMAT_SD";
    constexpr const static char *FORMAT_SD_ACK = "FORMAT_SD_ACK";
    constexpr const static char *FORMAT_SD_NAK = "FORMAT_SD_NAK";
}

enum class AstrOsSerialMessageType
{
    UNKNOWN,
    REGISTRATION_SYNC, // from web server
    REGISTRATION_SYNC_ACK,
    POLL_ACK,
    POLL_NAK,
    DEPLOY_CONFIG, // from web server
    DEPLOY_CONFIG_ACK,
    DEPLOY_CONFIG_NAK,
    DEPLOY_SCRIPT, // from web server
    DEPLOY_SCRIPT_ACK,
    DEPLOY_SCRIPT_NAK,
    RUN_SCRIPT, // from web server
    RUN_SCRIPT_ACK,
    RUN_SCRIPT_NAK,
    PANIC_STOP,  // from web server
    RUN_COMMAND, // from web server
    RUN_COMMAND_ACK,
    RUN_COMMAND_NAK,
    FORMAT_SD, // from web server
    FORMAT_SD_ACK,
    FORMAT_SD_NAK,
};

typedef struct
{
    std::string name;
    std::string mac;
    std::string fingerprint;
} astros_peer_data_t;

typedef struct
{
    std::string msgId;
    AstrOsSerialMessageType type;
    bool valid;
} astros_serial_msg_validation_t;

class AstrOsSerialMessageService
{
private:
    std::string generateHeader(AstrOsSerialMessageType type, std::string msgId);
    std::map<AstrOsSerialMessageType, std::string> msgTypeMap;

public:
    AstrOsSerialMessageService();
    ~AstrOsSerialMessageService();

    astros_serial_msg_validation_t validateSerialMsg(std::string msg);

    std::string getRegistrationSyncAck(std::string msgId, std::vector<astros_peer_data_t> controllers);
    std::string getPollAck(std::string macAddress, std::string controller, std::string fingerprint);
    std::string getPollNak(std::string macAddress, std::string controller);
    std::string getBasicAckNak(AstrOsSerialMessageType type, std::string msgId, std::string macAddress, std::string controller, std::string data);

    // methods for testing, these messages are generated by the web server
    std::string getRegistrationSync(std::string msgId);
    std::string getDeployConfig(std::string msgId, std::vector<std::string> macs, std::vector<std::string> controllers, std::vector<std::string> configs);
    std::string getDeployScript(std::string msgId, std::string scriptId, std::vector<std::string> controllers, std::vector<std::string> scripts);
    std::string getRunScript(std::string msgId, std::string scriptId);
    std::string getRunCommand(std::string msgId, std::string controller, std::string command);
};

#endif
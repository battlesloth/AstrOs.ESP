#ifndef ASTROSSERIALMESSAGESERVICE_H
#define ASTROSSERIALMESSAGESERVICE_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

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
    constexpr const static char *SERVO_TEST = "SERVO_TEST";
    constexpr const static char *SERVO_TEST_ACK = "SERVO_TEST_ACK";
    constexpr const static char *FW_TRANSFER_BEGIN = "FW_TRANSFER_BEGIN";
    constexpr const static char *FW_TRANSFER_BEGIN_ACK = "FW_TRANSFER_BEGIN_ACK";
    constexpr const static char *FW_CHUNK = "FW_CHUNK";
    constexpr const static char *FW_CHUNK_ACK = "FW_CHUNK_ACK";
    constexpr const static char *FW_CHUNK_NAK = "FW_CHUNK_NAK";
    constexpr const static char *FW_TRANSFER_END = "FW_TRANSFER_END";
    constexpr const static char *FW_TRANSFER_END_ACK = "FW_TRANSFER_END_ACK";
    constexpr const static char *FW_DEPLOY_BEGIN = "FW_DEPLOY_BEGIN";
    constexpr const static char *FW_PROGRESS = "FW_PROGRESS";
    constexpr const static char *FW_DEPLOY_DONE = "FW_DEPLOY_DONE";
    constexpr const static char *FW_BACKPRESSURE = "FW_BACKPRESSURE";
} // namespace AstrOsSC

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
    SERVO_TEST,
    SERVO_TEST_ACK,
    // Values 23–29 are reserved for in-flight non-OTA additions per
    // .docs/protocol.md. FW_* OTA types start at 30.
    FW_TRANSFER_BEGIN = 30,
    FW_TRANSFER_BEGIN_ACK = 31,
    FW_CHUNK = 32,
    FW_CHUNK_ACK = 33,
    FW_CHUNK_NAK = 34,
    FW_TRANSFER_END = 35,
    FW_TRANSFER_END_ACK = 36,
    FW_DEPLOY_BEGIN = 37,
    FW_PROGRESS = 38,
    FW_DEPLOY_DONE = 39,
    FW_BACKPRESSURE = 40,
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
    // Payload group (everything after GROUP_SEPARATOR). Empty when the
    // validated message carries no payload, e.g. REGISTRATION_SYNC.
    std::string payload;
    AstrOsSerialMessageType type;
    bool valid;
} astros_serial_msg_validation_t;

typedef struct
{
    std::string controllerId;
    std::string status;       // "OK" or "FAILED"
    std::string finalVersion; // may be empty
    std::string errorOrEmpty; // may be empty
} astros_fw_deploy_result_t;

typedef struct
{
    std::string transferId;
    uint32_t totalSize;
    std::string sha256Hex;
    uint16_t chunkSize;
    std::vector<std::string> targetIds;
    bool valid;
} FwTransferBeginRecord;

typedef struct
{
    std::string transferId;
    uint32_t seq;
    uint16_t payloadLen;
    std::string base64Payload; // not decoded here — Phase 3 MIXED handler decodes
    uint16_t crc16;
    bool valid;
} FwChunkRecord;

typedef struct
{
    std::string transferId;
    uint32_t totalChunks;
    std::string finalSha256Hex;
    bool valid;
} FwTransferEndRecord;

typedef struct
{
    std::string transferId;
    std::vector<std::string> orderIds;
    bool valid;
} FwDeployBeginRecord;

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
    std::string getPollAck(std::string macAddress, std::string controller, std::string fingerprint,
                           std::string firmwareVersion, std::string variant);
    std::string getPollNak(std::string macAddress, std::string controller);
    std::string getBasicAckNak(AstrOsSerialMessageType type, std::string msgId, std::string macAddress,
                               std::string controller, std::string data);

    // FW_* outbound builders (master → server)
    std::string getFwTransferBeginAck(std::string msgId, std::string transferId, std::string status);
    std::string getFwChunkAck(std::string transferId, uint32_t highestContiguousSeq, uint32_t nextExpectedSeq,
                              uint8_t windowRemaining);
    std::string getFwChunkNak(std::string transferId, uint32_t lastGoodSeq, uint32_t nextExpectedSeq,
                              std::string reasonCode);
    std::string getFwTransferEndAck(std::string msgId, std::string transferId, std::string status,
                                    std::string computedSha256Hex);
    std::string getFwDeployDone(std::string msgId, std::string transferId,
                                std::vector<astros_fw_deploy_result_t> results);
    // FW_PROGRESS wire format:
    //   FW_PROGRESS<header>transferId<US>controllerId<US>stage<US>bytesSent<US>totalBytes<US>detail
    // stage is a literal string from the FwStage enum on the server side
    // (e.g. "SENDING", "VERIFYING", "FLASHING"). The server parses parts[2]
    // as the stage string (see astros_api/src/serial/message_handler.ts
    // handleFwProgress), so the wire-encoding here is the string itself,
    // not a numeric id.
    std::string getFwProgress(std::string msgId, std::string transferId, std::string controllerId, std::string stage,
                              uint32_t bytesSent, uint32_t totalBytes, std::string detail);

    // methods for testing, these messages are generated by the web server
    std::string getRegistrationSync(std::string msgId);
    std::string getDeployConfig(std::string msgId, std::vector<std::string> macs, std::vector<std::string> controllers,
                                std::vector<std::string> configs);
    std::string getDeployScript(std::string msgId, std::string scriptId, std::vector<std::string> controllers,
                                std::vector<std::string> scripts);
    std::string getRunScript(std::string msgId, std::string scriptId);
    std::string getRunCommand(std::string msgId, std::string controller, std::string command);
    std::string getPanicStop(std::string msgId);
    std::string getFormatSD(std::string msgId);
    std::string getServoTest(std::string msgId, std::string macAddress, std::string controller, std::string data);
};

// Free parsers for inbound FW_* payloads. Live alongside the
// AstrOsSerialMessageService class because they share the wire
// grammar in this file. Pure C++; no allocations beyond the
// returned struct's members.
FwTransferBeginRecord parseFwTransferBegin(const std::string &payload);
FwChunkRecord parseFwChunk(const std::string &payload);
FwTransferEndRecord parseFwTransferEnd(const std::string &payload);
FwDeployBeginRecord parseFwDeployBegin(const std::string &payload);

#endif

#ifndef ASTROSSERVERRESPONSEMSG_H
#define ASTROSSERVERRESPONSEMSG_H

#include <string>

enum class AstrOsInterfaceResponseType
{
    UNKNOWN,
    REGISTRATION_SYNC,
    SEND_POLL_ACK,
    SEND_POLL_NAK,
    SET_CONFIG,
    SEND_CONFIG,
    SEND_CONFIG_ACK,
    SEND_CONFIG_NAK,
    SAVE_SCRIPT,
    SEND_SCRIPT,
    SAVE_SCRIPT_ACK,
    SAVE_SCRIPT_NAK,
    SCRIPT_RUN,
    SEND_SCRIPT_RUN,
    SCRIPT_RUN_ACK,
    SCRIPT_RUN_NAK,
    PANIC_STOP,
    SEND_PANIC_STOP,
    FORMAT_SD,
    SEND_FORMAT_SD,
    FORMAT_SD_ACK,
    FORMAT_SD_NAK,
    COMMAND,
    SEND_COMMAND,
    COMMAND_ACK,
    COMMAND_NAK,
    SERVO_TEST,
    SEND_SERVO_TEST,
    SERVO_TEST_ACK,
    SEND_SERVO_TEST_ACK
};

typedef struct
{
    AstrOsInterfaceResponseType type;
    char *originationMsgId;
    char *peerMac;
    char *peerName;
    char *message;
} astros_interface_response_t;

#endif
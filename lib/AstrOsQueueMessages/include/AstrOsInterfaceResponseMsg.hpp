#ifndef ASTROSSERVERRESPONSEMSG_H
#define ASTROSSERVERRESPONSEMSG_H

#include <string>

enum class AstrOsInterfaceResponseType
{
    UNKOWN,
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
    SEND_SCRIPT_RUN_ACK,
    SEND_SCRIPT_RUN_NAK,
    PANIC_STOP,
    SEND_PANIC_STOP,
    FORMAT_SD,
    SEND_FORMAT_SD,
    SEND_FORMAT_SD_ACK,
    SEND_FORMAT_SD_NAK,
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
#ifndef ASTROSSERVERRESPONSEMSG_H
#define ASTROSSERVERRESPONSEMSG_H

#include <string>

enum class AstrOsInterfaceResponseType
{
    UNKOWN,
    REGISTRATION_SYNC,
    SET_CONFIG,
    SEND_CONFIG,
    SEND_CONFIG_ACK,
    SEND_CONFIG_NAK,
    SAVE_SCRIPT,
    SEND_SCRIPT,
    SAVE_SCRIPT_ACK,
    SAVE_SCRIPT_NAK,
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
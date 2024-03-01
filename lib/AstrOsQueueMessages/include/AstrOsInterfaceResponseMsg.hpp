#ifndef ASTROSSERVERRESPONSEMSG_H
#define ASTROSSERVERRESPONSEMSG_H

#include <string>

enum class AstrOsInterfaceResponseType
{
    REGISTRATION_SYNC,
    SET_CONFIG,
    SEND_CONFIG,
    SEND_CONFIG_ACK,
    SEND_CONFIG_NAK,
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
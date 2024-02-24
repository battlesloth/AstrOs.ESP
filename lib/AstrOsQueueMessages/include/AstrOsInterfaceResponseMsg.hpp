#ifndef ASTROSSERVERRESPONSEMSG_H
#define ASTROSSERVERRESPONSEMSG_H

#include <string>

enum class AstrOsInterfaceResponseType
{
    REGISTRATION_SYNC,
    SET_CONFIG,
    SEND_CONFIG
};

typedef struct
{
    AstrOsInterfaceResponseType type;
    char *originationMsgId;
    char *peer;
    char *message;
} astros_interface_response_t;

#endif
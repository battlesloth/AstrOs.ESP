#ifndef ASTROSSERVERRESPONSEMSG_H
#define ASTROSSERVERRESPONSEMSG_H

#include <string>

enum class AstrOsServerResponseType
{
    REGISTRATION_SYNC,
};

typedef struct
{
    AstrOsServerResponseType type;
    char *originationMsgId;
    char *message;
} astros_server_response_t;

#endif
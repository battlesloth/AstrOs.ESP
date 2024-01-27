#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#include "AstrOsEnums.h"
#include <stdio.h>
#include <stdbool.h>

    typedef struct
    {
        SERVICE_COMMAND cmd;
        char data[100];
    } queue_svc_cmd_t;

    typedef struct
    {
        HARDWARE_COMMAND cmd;
        char data[100];
    } queue_hw_cmd_t;

    typedef struct
    {
        ANIMATION_COMMAND cmd;
        char data[100];
    } queue_ani_cmd_t;

    typedef struct
    {
        ASTROS_COMMAND command;
        uint8_t *body;
    } serial_cmd_t;

    typedef struct
    {
        char networkSSID[33];
        char networkPass[65];
    } svc_config_t;

    typedef struct
    {
        int message_id;
        char data[100];
    } queue_msg_t;

    typedef struct
    {
        int id;
        int minPos;
        int maxPos;
        int moveFactor;
        int currentPos;
        int requestedPos;
        int speed;
        bool set;
        bool inverted;
        bool on;
    } servo_channel;

#ifdef __cplusplus
}
#endif
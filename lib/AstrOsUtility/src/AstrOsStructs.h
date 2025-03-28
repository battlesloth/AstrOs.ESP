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
        uint8_t *data;
        size_t dataSize;
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
        uint8_t masterMacAddress[6];
        char name[16];
        char fingerprint[37];
    } svc_config_t;

    typedef struct
    {
        int message_id;
        uint8_t *data;
        size_t dataSize;
    } queue_msg_t;

    typedef struct
    {
        // 0 = send command, 1 = send bytes
        int message_id;
        int baudrate;
        uint8_t *data;
        size_t dataSize;
    } queue_serial_msg_t;

    typedef struct 
    {
        int idx;
        int uartChannel;
        int baudrate;
    } maestro_config;
    
    typedef struct
    {
        int id;
        bool enabled;
        bool isServo;
        int minPos;
        int maxPos;
        int home;
        int currentPos;
        int requestedPos;
        int lastPos;
        int speed;
        int acceleration;
        bool inverted;
        bool on;
    } servo_channel;

#ifdef __cplusplus
}
#endif
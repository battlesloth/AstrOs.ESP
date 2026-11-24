#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#include "AstrOsEnums.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

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

// Default per-channel release time in ms (T-010, .docs/protocol.md § D).
// A macro so this C-visible header can use it; MAESTRO_RELEASE_DEFAULT_MS
// (AstrOsServoUtils.hpp) is defined from it.
#define ASTROS_MAESTRO_RELEASE_DEFAULT_MS 5000

    // Tagged so the C++-only member default below is valid for a typedef'd struct.
    typedef struct servo_channel
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
        // Minimum ms the channel stays powered after a move (T-010). C++ code
        // (every user of this struct) gets the default for any `{}`- or
        // default-initialized channel, never 0 ms (a memset one reads 0, and
        // ServoReleaseDeadlineMs then clamps it to the default); C sees the same layout.
        int releaseMs
#ifdef __cplusplus
            = ASTROS_MAESTRO_RELEASE_DEFAULT_MS
#endif
            ;
    } servo_channel;

#ifdef __cplusplus
}
#endif
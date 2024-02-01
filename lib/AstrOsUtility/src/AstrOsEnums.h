#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        START_WIFI_AP,
        STOP_WIFI_AP,
        CONNECT_TO_NETWORK,
        DISCONNECT_FROM_NETWORK,
        SWITCH_TO_NETWORK,
        SWITCH_TO_WIFI_AP,
        SWITCH_TO_DISCOVERY,
        SWITCH_TO_ESPNOW,
        ESPNOW_DISCOVERY_MODE_ON,
        ESPNOW_DISCOVERY_MODE_OFF
    } SERVICE_COMMAND;

    typedef enum
    {
        MOVE_SERVO,
        LOAD_SERVO_CONFIG,
        SEND_SERIAL,
        SEND_I2C,
        DISPLAY_COMMAND
    } HARDWARE_COMMAND;

    typedef enum
    {
        PANIC_STOP,
        RUN_ANIMATION
    } ANIMATION_COMMAND;

    typedef enum
    {
        CONFIG,
        FUNCTION,
        SCRIPT,
        SERVO,
    } ASTROS_COMMAND;

#ifdef __cplusplus
}
#endif
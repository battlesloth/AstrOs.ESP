#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        NONE,
        MAESTRO,
        I2C,
        GENERIC_SERIAL,
        KANGAROO,
        GPIO
    } MODULE_TYPE;

    typedef enum
    {
        SHOW_DISPLAY,
        ESPNOW_DISCOVERY_MODE_ON,
        ESPNOW_DISCOVERY_MODE_OFF,
        ASTROS_INTERFACE_MESSAGE,
        RELOAD_CONFIG,
        FORMAT_SD
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
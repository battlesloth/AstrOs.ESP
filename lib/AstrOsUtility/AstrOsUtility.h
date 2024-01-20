#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdio.h>
#include <stdbool.h>
#include <esp_err.h>
#include <esp_now.h>

    typedef enum
    {
        START_WIFI_AP,
        STOP_WIFI_AP,
        CONNECT_TO_NETWORK,
        DISCONNECT_FROM_NETWORK,
        SWITCH_TO_NETWORK,
        SWITCH_TO_WIFI_AP,
        SWITCH_TO_DISCOVERY,
        SWITCH_TO_ESPNOW
    } SERVICE_COMMAND;

    typedef struct
    {
        SERVICE_COMMAND cmd;
        char data[100];
    } queue_svc_cmd_t;

    typedef enum
    {
        MOVE_SERVO,
        LOAD_SERVO_CONFIG,
        SEND_SERIAL,
        SEND_I2C,
        DISPLAY_COMMAND
    } HARDWARE_COMMAND;

    typedef struct
    {
        HARDWARE_COMMAND cmd;
        char data[100];
    } queue_hw_cmd_t;

    typedef enum
    {
        PANIC_STOP,
        RUN_ANIMATION
    } ANIMATION_COMMAND;

    typedef struct
    {
        ANIMATION_COMMAND cmd;
        char data[100];
    } queue_ani_cmd_t;

    typedef enum
    {
        CONFIG,
        FUNCTION,
        SCRIPT,
        SERVO,
    } ASTROS_COMMAND;

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

/***************
 * ESP-NOW
 ****************/
#define IS_BROADCAST_ADDR(addr) (memcmp(addr, broadcast_mac, ESP_NOW_ETH_ALEN) == 0)

    typedef enum
    {
        ESPNOW_SEND,
        ESPNOW_RECV,
        ESPNOW_DISCOVERY_MODE_ON,
        ESPNOW_DISCOVERY_MODE_OFF
    } espnow_event_id_t;

    /* When ESPNOW sending or receiving callback function is called, post event to ESPNOW task. */
    typedef struct
    {
        espnow_event_id_t id;
        uint8_t src[ESP_NOW_ETH_ALEN];
        uint8_t dest[ESP_NOW_ETH_ALEN];
        esp_now_send_status_t status;
        uint8_t *data;
        int data_len;
    } queue_espnow_msg_t;

    /*typedef struct
    {
        uint8_t mac_addr[ESP_NOW_ETH_ALEN];
        esp_now_send_status_t status;
    } espnow_event_send_cb_t;

    typedef struct
    {
        uint8_t mac_addr[ESP_NOW_ETH_ALEN];
        uint8_t *data;
        int data_len;
    } espnow_event_recv_cb_t;
*/

    /**********************************
     * Helper Methods
     *********************************/

    // if the esp_err_t != ESP_OK, log the error with the function and line number
    bool logError(const char *tag, const char *function, int line, esp_err_t err);

    // decode url encoded strings
    int percentDecode(char *out, const char *in);

#ifdef __cplusplus
}
#endif
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
        ESPNOW_SEND_CB,
        ESPNOW_RECV_CB,
    } espnow_event_id_t;

    enum
    {
        ESPNOW_DATA_BROADCAST,
        ESPNOW_DATA_UNICAST,
        ESPNOW_DATA_MAX,
    };

    typedef struct
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

    typedef union
    {
        espnow_event_send_cb_t send_cb;
        espnow_event_recv_cb_t recv_cb;
    } espnow_event_info_t;

    /* When ESPNOW sending or receiving callback function is called, post event to ESPNOW task. */
    typedef struct
    {
        espnow_event_id_t id;
        espnow_event_info_t info;
    } espnow_event_t;

    /* User defined field of ESPNOW data in this example. */
    typedef struct
    {
        uint8_t type;       // Broadcast or unicast ESPNOW data.
        uint8_t state;      // Indicate that if has received broadcast ESPNOW data or not.
        uint16_t seq_num;   // Sequence number of ESPNOW data.
        uint16_t crc;       // CRC16 value of ESPNOW data.
        uint32_t magic;     // Magic number which is used to determine which device to send unicast ESPNOW data.
        uint8_t payload[0]; // Real payload of ESPNOW data.
    } __attribute__((packed)) espnow_data_t;

    /* Parameters of sending ESPNOW data. */
    typedef struct
    {
        bool unicast;                       // Send unicast ESPNOW data.
        bool broadcast;                     // Send broadcast ESPNOW data.
        uint8_t state;                      // Indicate that if has received broadcast ESPNOW data or not.
        uint32_t magic;                     // Magic number which is used to determine which device to send unicast ESPNOW data.
        uint16_t count;                     // Total count of unicast ESPNOW data to be sent.
        uint16_t delay;                     // Delay between sending two ESPNOW data, unit: ms.
        int len;                            // Length of ESPNOW data to be sent, unit: byte.
        uint8_t *buffer;                    // Buffer pointing to ESPNOW data.
        uint8_t dest_mac[ESP_NOW_ETH_ALEN]; // MAC address of destination device.
    } espnow_send_param_t;

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
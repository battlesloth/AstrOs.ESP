#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#include <esp_now.h>

#define CONFIG_ESPNOW_PMK "pmk1234567890123"
#define CONFIG_ESPNOW_LMK "lmk1234567890123"
#define ESPNOW_CHANNEL 1
#define ESPNOW_CHANNEL 1
#define ESPNOW_SEND_COUNT 100
#define ESPNOW_SEND_DELAY 1000
#define ESPNOW_SEND_LEN 200
#define ESPNOW_WIFI_MODE WIFI_MODE_STA
#define ESPNOW_WIFI_IF WIFI_IF_STA
#define ESPNOW_MAXDELAY 512
#define ESPNOW_PEER_LIMIT 10

#define IS_BROADCAST_ADDR(addr) (memcmp(addr, broadcastMac, ESP_NOW_ETH_ALEN) == 0)
#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"

    // static uint8_t broadcastMac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    // static uint8_t nullMac[ESP_NOW_ETH_ALEN] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    typedef enum
    {
        ESPNOW_SEND,
        ESPNOW_RECV,
        SEND_REGISTRAION_REQ,
        POLL_PADAWANS,
        EXPIRE_POLLS
    } EspNowQueueEventType;

    /* When ESPNOW sending or receiving callback function is called, post event to ESPNOW task. */
    typedef struct
    {
        EspNowQueueEventType eventType;
        uint8_t src[ESP_NOW_ETH_ALEN];
        uint8_t dest[ESP_NOW_ETH_ALEN];
        esp_now_send_status_t status;
        uint8_t *data;
        int data_len;
    } queue_espnow_msg_t;

    typedef struct
    {
        int id;
        char name[16];
        uint8_t mac_addr[ESP_NOW_ETH_ALEN];
        char crypto_key[16];
        bool is_paired;
        bool pollAckThisCycle;
    } espnow_peer_t;

#ifdef __cplusplus
}
#endif
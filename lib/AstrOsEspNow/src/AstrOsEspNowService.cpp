#include <AstrOsEspNowService.h>
#include <AstrOsUtility_Esp.h>
#include <AstrOsMessaging.h>

#include <esp_err.h>
#include <esp_now.h>
#include <esp_log.h>
#include <string.h>

static const char *TAG = "AstrOsEspNow";

AstrOsEspNow AstrOs_EspNow;

AstrOsEspNow::AstrOsEspNow()
{
}

AstrOsEspNow::~AstrOsEspNow()
{
}

esp_err_t AstrOsEspNow::init()
{
    return ESP_OK;
}

esp_err_t AstrOsEspNow::addPeer(uint8_t *macAddress)
{
    esp_err_t err = ESP_OK;

    esp_now_peer_info_t *peer = (esp_now_peer_info_t *)malloc(sizeof(esp_now_peer_info_t));

    memset(peer, 0, sizeof(esp_now_peer_info_t));
    peer->channel = ESPNOW_CHANNEL;
    peer->ifidx = ESPNOW_WIFI_IF;
    peer->encrypt = false;
    // TODO: implement encryption
    //  memcpy(peer->lmk, CONFIG_ESPNOW_LMK, ESP_NOW_KEY_LEN);
    memcpy(peer->peer_addr, macAddress, ESP_NOW_ETH_ALEN);
    err = esp_now_add_peer(peer);
    logError(TAG, __FUNCTION__, __LINE__, err);
    free(peer);

    return err;
}

queue_espnow_msg_t AstrOsEspNow::generateRegisterMessage(uint8_t *macAddress)
{
    queue_espnow_msg_t regMsg;
    regMsg.eventType = ESPNOW_SEND;
    memcpy(regMsg.dest, macAddress, ESP_NOW_ETH_ALEN);
    regMsg.data = (uint8_t *)malloc(16);
    regMsg.data_len = 16;
    memcpy(regMsg.data, "AstrOs Register", 16);

    return regMsg;
}

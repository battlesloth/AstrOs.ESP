#ifndef ASTROSESPNOWSERVICE_H
#define ASTROSESPNOWSERVICE_H

#include "AstrOsEspNowUtility.h"
#include "AstrOsMessaging.h"
#include <esp_err.h>

// needed for QueueHandle_t, must be in this order
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

typedef struct
{
    uint8_t *masterMac;
    std::string name;
    bool isMaster;
    espnow_peer_t *peers;
    int peerCount;
    QueueHandle_t serviceQueue;
} astros_espnow_config_t;

class AstrOsEspNow
{
private:
    uint8_t masterMac[ESP_NOW_ETH_ALEN];
    bool isMasterNode;
    std::string mac;
    std::vector<espnow_peer_t> peers;
    QueueHandle_t serviceQueue;

    void getMasterMac(uint8_t *macAddress);
    void updateMasterMac(u_int8_t *macAddress);

    esp_err_t addPeer(uint8_t *macAddress);
    bool cachePeer(u_int8_t *macAddress, std::string name);

    bool (*cachePeerCallback)(espnow_peer_t);
    void (*updateSeviceConfigCallback)(std::string, uint8_t *);
    void (*displayUpdateCallback)(std::string, std::string, std::string);

    queue_espnow_msg_t generatePacket(AstrOsPacketType type, uint8_t *data, uint8_t data_len);
    astros_packet_t parsePacket(uint8_t *data);

    bool handleRegistrationReq(u_int8_t *src);
    bool sendRegistration(u_int8_t *macAddress, std::string name);
    bool handleRegistration(u_int8_t *src, u_int8_t *payload, size_t len);
    bool sendRegistrationAck();
    bool handleRegistrationAck(u_int8_t *src, u_int8_t *payload, size_t len);
    bool handleHeartbeat(astros_packet_t packet);

public:
    AstrOsEspNow();
    ~AstrOsEspNow();
    std::string name;
    esp_err_t init(astros_espnow_config_t config,
                   bool (*cachePeer_cb)(espnow_peer_t),
                   void (*displayUpdate_cb)(std::string, std::string, std::string),
                   void (*updateSeviceConfig_cb)(std::string, uint8_t *));
    void sendHeartbeat(bool discoveryMode);
    bool handleMessage(u_int8_t *src, u_int8_t *data, size_t len);
};

extern AstrOsEspNow AstrOs_EspNow;

#endif
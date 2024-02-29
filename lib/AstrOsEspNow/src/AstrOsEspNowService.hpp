#ifndef ASTROSESPNOWSERVICE_HPP
#define ASTROSESPNOWSERVICE_HPP

#include "AstrOsEspNowUtility.h"
#include "AstrOsMessaging.hpp"
#include <esp_err.h>

// needed for QueueHandle_t, must be in this order
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

static uint8_t broadcastMac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static uint8_t nullMac[ESP_NOW_ETH_ALEN] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

typedef struct
{
    uint8_t *masterMac;
    std::string name;
    std::string fingerprint;
    bool isMaster;
    espnow_peer_t *peers;
    int peerCount;
    QueueHandle_t serviceQueue;
    QueueHandle_t interfaceQueue;
    void (*espnowSend_cb)(const uint8_t *mac_addr, esp_now_send_status_t status);
    void (*espnowRecv_cb)(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len);
    bool (*cachePeer_cb)(espnow_peer_t);
    void (*updateSeviceConfig_cb)(std::string, uint8_t *);
    void (*displayUpdate_cb)(std::string, std::string, std::string);
} astros_espnow_config_t;

class AstrOsEspNow
{
private:
    std::string name;
    std::string fingerprint;
    uint8_t masterMac[ESP_NOW_ETH_ALEN];
    bool isMasterNode;
    std::string mac;
    std::vector<espnow_peer_t> peers;
    QueueHandle_t serviceQueue;
    QueueHandle_t interfaceQueue;

    PacketTracker packetTracker;

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
    bool handlePoll(astros_packet_t packet);
    bool handlePollAck(astros_packet_t packet);
    bool handleConfig(astros_packet_t packet);

    std::string handleMultiPacketMessage(astros_packet_t packet);
    esp_err_t wifiInit(void);
    esp_err_t espnowInit(void);

public:
    AstrOsEspNow();
    ~AstrOsEspNow();
    esp_err_t init(astros_espnow_config_t config);
    std::vector<espnow_peer_t> getPeers();
    void sendRegistrationRequest();
    bool handleMessage(u_int8_t *src, u_int8_t *data, size_t len);
    void pollPadawans();
    void pollRepsonseTimeExpired();
    void sendConfigUpdate(std::string peer, std::string msgId, std::string msg);
    void sendConfigAckNak(std::string msgId, bool success);

    std::string getMac();
    std::string getName();
    std::string getFingerprint();
    void updateFingerprint(std::string fingerprint);
};

extern AstrOsEspNow AstrOs_EspNow;

#endif
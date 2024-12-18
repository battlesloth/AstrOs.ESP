#ifndef ASTROSESPNOWSERVICE_HPP
#define ASTROSESPNOWSERVICE_HPP

#include "AstrOsEspNowUtility.h"
#include "AstrOsMessaging.hpp"
#include <AstrOsInterfaceResponseMsg.hpp>
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

    AstrOsEspNowMessageService messageService;

    void getMasterMac(uint8_t *macAddress);
    void updateMasterMac(uint8_t *macAddress);

    esp_err_t addPeer(uint8_t *macAddress);
    bool cachePeer(uint8_t *macAddress, std::string name);
    bool findPeer(std::string peer);
    bool isValidPollPeer(std::string peer);

    bool (*cachePeerCallback)(espnow_peer_t);
    void (*updateSeviceConfigCallback)(std::string, uint8_t *);
    void (*displayUpdateCallback)(std::string, std::string, std::string);

    void sendEspNowMessage(AstrOsPacketType type, std::string peer, std::string msg);
    void sendToInterfaceQueue(AstrOsInterfaceResponseType responseType, std::string peerMac,
                              std::string peerName, std::string msgId, std::string message);

    bool handleRegistrationReq(uint8_t *src);
    bool sendRegistration(uint8_t *macAddress, std::string name);
    bool handleRegistration(uint8_t *src, astros_packet_t packet);
    bool sendRegistrationAck();
    bool handleRegistrationAck(uint8_t *src, astros_packet_t packet);
    bool handlePoll(astros_packet_t packet);
    bool handlePollAck(astros_packet_t packet);
    bool handleConfig(astros_packet_t packet);
    bool handleConfigAckNak(astros_packet_t packet);
    bool handleScriptDeploy(astros_packet_t packet);
    bool handleScriptRun(astros_packet_t packet);
    bool handleCommandRun(astros_packet_t packet);
    bool handlePanicStop(astros_packet_t packet);
    bool handleFormatSD(astros_packet_t packet);
    bool handleServoTest(astros_packet_t packet);

    bool handleBasicAckNak(astros_packet_t packet);
    AstrOsInterfaceResponseType getInterfaceResponseType(AstrOsPacketType type);

    std::string handleMultiPacketMessage(astros_packet_t packet);
    esp_err_t wifiInit(void);
    esp_err_t espnowInit(void);

public:
    AstrOsEspNow();
    ~AstrOsEspNow();
    esp_err_t init(astros_espnow_config_t config);
    std::vector<espnow_peer_t> getPeers();
    void sendRegistrationRequest();
    bool handleMessage(uint8_t *src, uint8_t *data, size_t len);
    void pollPadawans();
    void pollRepsonseTimeExpired();
    void sendConfigAckNak(std::string msgId, bool success);

    void sendBasicCommand(AstrOsPacketType type, std::string peer, std::string msgId, std::string msg);
    void sendBasicAckNak(std::string msgId, AstrOsPacketType type, std::string msg);

    std::string getMac();
    std::string getName();
    std::string getFingerprint();
    void updateFingerprint(std::string fingerprint);
};

extern AstrOsEspNow AstrOs_EspNow;

#endif
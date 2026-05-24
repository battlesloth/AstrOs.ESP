#ifndef ASTROSESPNOWSERVICE_HPP
#define ASTROSESPNOWSERVICE_HPP

#include "AstrOsEspNowUtility.h"
#include "AstrOsMessaging.hpp"
#include <AstrOsEspNowPeers.hpp>
#include <AstrOsInterfaceResponseMsg.hpp>
#include <esp_err.h>

// needed for QueueHandle_t and SemaphoreHandle_t, must be in this order
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

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
    void (*espnowSend_cb)(const esp_now_send_info_t *tx_info, esp_now_send_status_t status);
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
    AstrOsEspNowPeers::PeerList peers;
    SemaphoreHandle_t peersMutex;
    QueueHandle_t serviceQueue;
    QueueHandle_t interfaceQueue;

    PacketTracker packetTracker;

    AstrOsEspNowMessageService messageService;

    QueueHandle_t otaForwarderQueue_ = nullptr;

    // Routes an OTA ACK/NAK packet (master-side receive) into
    // otaForwarderQueue_. Parses via M1's parseOta* free functions to
    // distinguish wire-malformed (rec.valid == false) from
    // wire-valid-but-queue-full failures. Returns true on success;
    // false logs and drops.
    bool routeOtaAckNakToForwarder(const uint8_t *src, const astros_packet_t &packet);

    void getMasterMac(uint8_t *macAddress);
    void updateMasterMac(uint8_t *macAddress);

    esp_err_t addPeer(uint8_t *macAddress);
    bool cachePeer(uint8_t *macAddress, std::string name);
    bool findPeer(std::string peer);

    bool (*cachePeerCallback)(espnow_peer_t);
    void (*updateSeviceConfigCallback)(std::string, uint8_t *);
    void (*displayUpdateCallback)(std::string, std::string, std::string);

    void sendEspNowMessage(AstrOsPacketType type, std::string peer, std::string msg);
    void sendToInterfaceQueue(AstrOsInterfaceResponseType responseType, std::string peerMac, std::string peerName,
                              std::string msgId, std::string message);

    bool handleRegistrationReq(uint8_t *src);
    bool sendRegistration(uint8_t *macAddress, std::string name);
    bool handleRegistration(uint8_t *src, astros_packet_t packet);
    bool sendRegistrationAck();
    bool handleRegistrationAck(uint8_t *src, astros_packet_t packet);
    bool handlePoll(astros_packet_t packet);
    bool handlePollAck(astros_packet_t packet);

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

    // Binary-frame TX for OTA. Builds the wire frame via
    // messageService.generateOtaPacket(type, payload, len), unicasts to
    // `mac` via esp_now_send. Returns ESP_OK on successful enqueue;
    // ESP_ERR_INVALID_ARG if the builder rejected (wrong type / oversize);
    // whatever esp_now_send returned otherwise.
    //
    // Memory ownership: `payload` is copied into the wire frame inside
    // generateOtaPacket; caller retains ownership of the input buffer.
    // The internal frame buffer is freed unconditionally after esp_now_send
    // returns (Pattern A immediate-free, matching the other send-helpers).
    esp_err_t sendOtaFrame(const uint8_t mac[6], AstrOsPacketType type, const uint8_t *payload, size_t len);

    // OTA ACK/NAK arrivals on the master are routed into this queue.
    // Called from main.cpp during init; before this is set, OTA arrivals
    // on master fall through to ESP_LOGW + drop (same path as padawan).
    void setOtaForwarderQueue(QueueHandle_t q);

    std::string getMac();
    std::string getName();
    std::string getFingerprint();
    void updateFingerprint(std::string fingerprint);
};

extern AstrOsEspNow AstrOs_EspNow;

#endif
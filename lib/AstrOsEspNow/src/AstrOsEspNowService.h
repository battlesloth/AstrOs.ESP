#ifndef ASTROSESPNOWSERVICE_H
#define ASTROSESPNOWSERVICE_H

#include "AstrOsEspNowUtility.h"
#include "AstrOsMessaging.h"
#include <esp_err.h>

typedef struct
{
    uint8_t *masterMac;
    std::string name;
    bool isMaster;
    espnow_peer_t *peers;
    int peerCount;
} astros_espnow_config_t;

class AstrOsEspNow
{
private:

    uint8_t masterMac[ESP_NOW_ETH_ALEN];
    bool isMasterNode;
    std::string name;
    std::string mac;
    std::vector<espnow_peer_t> peers;

    void getMasterMac(uint8_t *macAddress);
    void updateMasterMac(u_int8_t *macAddress);

    bool cachePeer(u_int8_t *macAddress, std::string name);
    bool (*cachePeerCallback)(espnow_peer_t);

    queue_espnow_msg_t generatePacket(AstrOsPacketType type, uint8_t *data, uint8_t data_len);
    astros_packet_t parsePacket(uint8_t *data);

    void sendRegistration(u_int8_t *macAddress, std::string name);
    void sendRegistrationAck();
    bool handleRegistrationReq(u_int8_t *src);
    bool handleRegistration(u_int8_t *src, u_int8_t *payload, size_t len);
    bool handleRegistrationAck(u_int8_t *src);
    bool handleHeartbeat(u_int8_t *src);

public:
    AstrOsEspNow();
    ~AstrOsEspNow();
    esp_err_t init(astros_espnow_config_t config, bool (*func_ptr)(espnow_peer_t));
    esp_err_t addPeer(uint8_t *macAddress);
    void sendHeartbeat(bool discoveryMode);
    bool handleMessage(u_int8_t *src, u_int8_t *data, size_t len);
};

extern AstrOsEspNow AstrOs_EspNow;

#endif
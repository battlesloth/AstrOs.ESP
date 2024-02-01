#include <AstrOsEspNowService.h>
#include <AstrOsUtility_Esp.h>
#include <AstrOsMessaging.h>

#include <esp_err.h>
#include <esp_now.h>
#include <esp_log.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_mac.h>

static const char *TAG = "AstrOsEspNow";
SemaphoreHandle_t masterMacMutex;

AstrOsEspNow AstrOs_EspNow;

AstrOsEspNow::AstrOsEspNow()
{
}

AstrOsEspNow::~AstrOsEspNow()
{
}

esp_err_t AstrOsEspNow::init(astros_espnow_config_t config, bool (*cachePeer_cb)(espnow_peer_t), void (*displayUpdate_cb)(std::string, std::string, std::string))
{
    ESP_LOGI(TAG, "Initializing AstrOsEspNow");

    esp_err_t err = ESP_OK;

    AstrOsEspNow::name = config.name;
    AstrOsEspNow::isMasterNode = config.isMaster;
    AstrOsEspNow::peers = std::vector<espnow_peer_t>(config.peerCount);
    AstrOsEspNow::cachePeerCallback = cachePeer_cb;
    AstrOsEspNow::displayUpdateCallback = displayUpdate_cb;

    uint8_t *localMac = (uint8_t *)malloc(ESP_NOW_ETH_ALEN);

    err = esp_read_mac(localMac, ESP_MAC_WIFI_STA);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    AstrOsEspNow::mac = AstrOsEspNow::macToString(localMac);

    free(localMac);

    masterMacMutex = xSemaphoreCreateMutex();

    if (masterMacMutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialize the master mac mutex");
        return ESP_FAIL;
    }

    memcpy(AstrOsEspNow::masterMac, config.masterMac, ESP_NOW_ETH_ALEN);

    // Add broadcast peer information to peer list.
    err = AstrOsEspNow::addPeer(broadcastMac);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    // Load current peer list.
    for (int i = 0; i < config.peerCount; i++)
    {
        err = AstrOsEspNow::addPeer(config.peers[i].mac_addr);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            return err;
        }
    }

    ESP_LOGI(TAG, "AstrOsEspNow initialized");

    return err;
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

bool AstrOsEspNow::cachePeer(u_int8_t *macAddress, std::string name)
{
    // add to peer cache
    espnow_peer_t newPeer;

    // peer id is 0 indexed
    newPeer.id = peers.size();
    memcpy(newPeer.name, name.c_str(), name.length() + 1);
    memcpy(newPeer.mac_addr, macAddress, ESP_NOW_ETH_ALEN);
    memset(newPeer.crypto_key, 0, ESP_NOW_KEY_LEN);
    newPeer.is_paired = true;

    peers.push_back(newPeer);

    return cachePeerCallback(newPeer);
}

void AstrOsEspNow::updateMasterMac(uint8_t *macAddress)
{
    bool macSet = false;
    while (!macSet)
    {
        if (xSemaphoreTake(masterMacMutex, 100 / portTICK_PERIOD_MS))
        {
            memcpy(masterMac, macAddress, ESP_NOW_ETH_ALEN);
            macSet = true;
            xSemaphoreGive(masterMacMutex);
        }
        else
        {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }
}

void AstrOsEspNow::sendRegistration(u_int8_t *macAddress, std::string name)
{
}

void AstrOsEspNow::sendRegistrationAck()
{
    uint8_t *destMac = (uint8_t *)malloc(ESP_NOW_ETH_ALEN);

    getMasterMac(destMac);

    if (IS_BROADCAST_ADDR(destMac))
    {
        ESP_LOGE(TAG, "Cannot send registration ack to broadcast address");
        free(destMac);
        return;
    }

    astros_espnow_data_t data = AstrOsEspNowMessageService::generateEspNowMsg(AstrOsPacketType::REGISTRATION_ACK, name, mac);

    if (esp_now_send(destMac, data.data, data.size) != ESP_OK)
    {
        ESP_LOGE(TAG, "Error sending registration ack");
    }

    free(data.data);
    free(destMac);
}

void AstrOsEspNow::sendHeartbeat(bool discoveryMode)
{
    uint8_t *destMac = (uint8_t *)malloc(ESP_NOW_ETH_ALEN);

    getMasterMac(destMac);

    // if master mac is not broadcast address, then assume we are registered
    if (!IS_BROADCAST_ADDR(destMac))
    {
        astros_espnow_data_t data = AstrOsEspNowMessageService::generateEspNowMsg(AstrOsPacketType::HEARTBEAT);
        if (esp_now_send(destMac, data.data, data.size) != ESP_OK)
        {
            ESP_LOGE(TAG, "Error sending heartbeat");
        }
        free(data.data);
    }
    // if master mac is broadcast address and we are in discovery mode, then send registration request
    else if (discoveryMode)
    {
        astros_espnow_data_t data = AstrOsEspNowMessageService::generateEspNowMsg(AstrOsPacketType::REGISTRATION_REQ);
        if (esp_now_send(destMac, data.data, data.size) != ESP_OK)
        {
            ESP_LOGE(TAG, "Error sending registraion request");
        }
        free(data.data);
    }

    free(destMac);
}

void AstrOsEspNow::getMasterMac(uint8_t *macAddress)
{
    bool macSet = false;
    while (!macSet)
    {
        if (xSemaphoreTake(masterMacMutex, 100 / portTICK_PERIOD_MS))
        {
            memcpy(macAddress, masterMac, ESP_NOW_ETH_ALEN);
            macSet = true;
            xSemaphoreGive(masterMacMutex);
        }
        else
        {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }
}

bool AstrOsEspNow::handleMessage(u_int8_t *src, u_int8_t *data, size_t len)
{
    astros_packet_t packet = AstrOsEspNowMessageService::parsePacket(data);

    switch (packet.packetType)
    {
    case AstrOsPacketType::REGISTRATION_REQ:
        if (!isMasterNode)
        {
            break;
        }
        return AstrOsEspNow::handleRegistrationReq(src);
    case AstrOsPacketType::REGISTRATION:
        if (isMasterNode)
        {
            break;
        }
        ESP_LOGI(TAG, "Registration received from " MACSTR, MAC2STR(src));
        return AstrOsEspNow::handleRegistration(src, packet.payload, packet.payloadSize);
    case AstrOsPacketType::REGISTRATION_ACK:
        if (!isMasterNode)
        {
            break;
        }
        ESP_LOGI(TAG, "Registration ack received from " MACSTR, MAC2STR(src));
        return AstrOsEspNow::handleRegistrationAck(src);
    case AstrOsPacketType::HEARTBEAT:
        if (!isMasterNode)
        {
            break;
        }
        ESP_LOGI(TAG, "Heartbeat received from " MACSTR, MAC2STR(src));
        return AstrOsEspNow::handleHeartbeat(src);
    default:
        return false;
    }

    return true;
}

bool AstrOsEspNow::handleRegistrationReq(u_int8_t *src)
{
    esp_err_t err = ESP_OK;

    if (!esp_now_is_peer_exist(src))
    {
        err = AstrOsEspNow::addPeer(src);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            return false;
        }
    }

    std::string macStr = AstrOsEspNow::macToString(src);
    std::string padewanName = "test";
    astros_espnow_data_t data = AstrOsEspNowMessageService::generateEspNowMsg(AstrOsPacketType::REGISTRATION, padewanName, macStr);

    err = esp_now_send(broadcastMac, data.data, data.size);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return false;
    }

    free(data.data);
    return true;
}

bool AstrOsEspNow::handleRegistration(u_int8_t *src, u_int8_t *payload, size_t len)
{
    esp_err_t err = ESP_OK;

    if (!esp_now_is_peer_exist(src))
    {
        err = AstrOsEspNow::addPeer(src);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            return false;
        }
    }

    std::string payloadStr(reinterpret_cast<char *>(payload), len);

    std::string name;
    std::string mac;

    std::string delimiter = "|";
    size_t pos = 0;
    std::string token;
    int i = 0;
    while ((pos = payloadStr.find(delimiter)) != std::string::npos)
    {
        token = payloadStr.substr(0, pos);
        if (i == 1)
        {
            name = token;
        }
        else if (i == 2)
        {
            mac = token;
        }
        payloadStr.erase(0, pos + delimiter.length());
        i++;
    }

    if (name.empty() || mac.empty())
    {
        ESP_LOGE(TAG, "Invalid registration payload: %s, %s", name.c_str(), mac.c_str());
        return false;
    }

    if (AstrOsEspNow::mac != mac)
    {
        ESP_LOGI(TAG, "Registraion received for different device: %s", mac.c_str());
    }

    AstrOsEspNow::name = name;
    AstrOsEspNow::updateMasterMac(src);
    AstrOsEspNow::displayUpdateCallback("Padawan", "", name);
    AstrOsEspNow::sendRegistrationAck();

    return true;
}

bool AstrOsEspNow::handleRegistrationAck(u_int8_t *src)
{
    return true;
}

bool AstrOsEspNow::handleHeartbeat(u_int8_t *src)
{
    return true;
}

std::string AstrOsEspNow::macToString(uint8_t *mac)
{
    char *macStr = (char *)malloc(17);
    sprintf(macStr, MACSTR, MAC2STR(mac));
    std::string macString(macStr, 17);
    free(macStr);
    return macString;
}
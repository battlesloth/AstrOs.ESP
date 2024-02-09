#include <AstrOsEspNowService.hpp>
#include <AstrOsUtility_Esp.h>
#include <AstrOsMessaging.h>
#include <AstrOsUtility.h>

#include <esp_err.h>
#include <esp_now.h>
#include <esp_log.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
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

esp_err_t AstrOsEspNow::init(astros_espnow_config_t config,
                             bool (*cachePeer_cb)(espnow_peer_t),
                             void (*displayUpdate_cb)(std::string, std::string, std::string),
                             void (*updateSeviceConfig_cb)(std::string, uint8_t *))
{
    ESP_LOGI(TAG, "Initializing AstrOsEspNow");

    esp_err_t err = ESP_OK;

    this->name = config.name;
    this->isMasterNode = config.isMaster;
    this->peers = std::vector<espnow_peer_t>(0);
    this->serviceQueue = config.serviceQueue;

    this->cachePeerCallback = cachePeer_cb;
    this->displayUpdateCallback = displayUpdate_cb;
    this->updateSeviceConfigCallback = updateSeviceConfig_cb;

    uint8_t *localMac = (uint8_t *)malloc(ESP_NOW_ETH_ALEN);

    err = esp_read_mac(localMac, ESP_MAC_WIFI_STA);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    this->mac = AstrOsStringUtils::macToString(localMac);

    free(localMac);

    masterMacMutex = xSemaphoreCreateMutex();

    if (masterMacMutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialize the master mac mutex");
        return ESP_FAIL;
    }

    memcpy(this->masterMac, config.masterMac, ESP_NOW_ETH_ALEN);

    // Add broadcast peer information to peer list.
    err = this->addPeer(broadcastMac);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    // Load current peer list.
    for (int i = 0; i < config.peerCount; i++)
    {

        auto peer = config.peers[i];

        err = this->addPeer(peer.mac_addr);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            return err;
        }

        peers.push_back(peer);
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

    if (peers.size() > 10)
    {
        ESP_LOGE(TAG, "Peer cache is full");
        return false;
    }

    for (auto &peer : peers)
    {
        if (memcmp(peer.mac_addr, macAddress, ESP_NOW_ETH_ALEN) == 0)
        {
            ESP_LOGI(TAG, "Peer already cached");
            return true;
        }
    }

    // add to peer cache
    espnow_peer_t newPeer;

    // peer id is 0 indexed
    newPeer.id = peers.size();
    memcpy(newPeer.name, name.c_str(), name.length() + 1);
    memcpy(newPeer.mac_addr, macAddress, ESP_NOW_ETH_ALEN);
    memset(newPeer.crypto_key, 0, ESP_NOW_KEY_LEN);
    newPeer.is_paired = true;

    peers.push_back(newPeer);

    return this->cachePeerCallback(newPeer);
}

void AstrOsEspNow::updateMasterMac(uint8_t *macAddress)
{
    bool macSet = false;
    while (!macSet)
    {
        if (xSemaphoreTake(masterMacMutex, 100 / portTICK_PERIOD_MS))
        {
            memcpy(this->masterMac, macAddress, ESP_NOW_ETH_ALEN);
            macSet = true;
            xSemaphoreGive(masterMacMutex);
        }
        else
        {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }
}

void AstrOsEspNow::getMasterMac(uint8_t *macAddress)
{
    bool macSet = false;
    while (!macSet)
    {
        if (xSemaphoreTake(masterMacMutex, 100 / portTICK_PERIOD_MS))
        {
            memcpy(macAddress, this->masterMac, ESP_NOW_ETH_ALEN);
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
        return this->handleRegistrationReq(src);
    case AstrOsPacketType::REGISTRATION:
        if (isMasterNode)
        {
            break;
        }
        ESP_LOGI(TAG, "Registration received from " MACSTR, MAC2STR(src));
        return this->handleRegistration(src, packet.payload, packet.payloadSize);
    case AstrOsPacketType::REGISTRATION_ACK:
        if (!isMasterNode)
        {
            break;
        }
        ESP_LOGI(TAG, "Registration ack received from " MACSTR, MAC2STR(src));
        return this->handleRegistrationAck(src, packet.payload, packet.payloadSize);
    case AstrOsPacketType::POLL:
        if (isMasterNode)
        {
            break;
        }
        ESP_LOGI(TAG, "Poll received from " MACSTR ", ", MAC2STR(src));
        return this->handlePoll(packet);
    case AstrOsPacketType::POLL_ACK:
        if (!isMasterNode)
        {
            break;
        }
        ESP_LOGI(TAG, "Poll received from " MACSTR ", ", MAC2STR(src));
        return this->handlePollAck(packet);

    default:
        return false;
    }

    return true;
}

void AstrOsEspNow::sendRegistrationRequest()
{
    uint8_t *destMac = (uint8_t *)malloc(ESP_NOW_ETH_ALEN);

    getMasterMac(destMac);

    // if master mac is broadcast address, then send registration request
    if (IS_BROADCAST_ADDR(destMac))
    {
        astros_espnow_data_t data = AstrOsEspNowMessageService::generateEspNowMsg(AstrOsPacketType::REGISTRATION_REQ);
        if (esp_now_send(destMac, data.data, data.size) != ESP_OK)
        {
            ESP_LOGE(TAG, "Error sending registraion request");
        }
        free(data.data);
    }
    // if we are already registered, then turn off discovery mode
    else
    {
        queue_svc_cmd_t cmd;
        cmd.cmd = SERVICE_COMMAND::ESPNOW_DISCOVERY_MODE_OFF;
        cmd.data = nullptr;

        if (xQueueSend(this->serviceQueue, &cmd, 100) != pdTRUE)
        {
            ESP_LOGE(TAG, "Send service queue fail");
        }
    }

    free(destMac);
}

bool AstrOsEspNow::handleRegistrationReq(u_int8_t *src)
{
    esp_err_t err = ESP_OK;

    if (!esp_now_is_peer_exist(src))
    {
        err = this->addPeer(src);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            return false;
        }
    }

    std::string macStr = AstrOsStringUtils::macToString(src);
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

    std::string payloadStr(reinterpret_cast<char *>(payload), len);

    std::string name;
    std::string mac;

    auto parts = AstrOsStringUtils::splitString(payloadStr, UNIT_SEPARATOR);

    if (parts.size() < 3)
    {
        ESP_LOGE(TAG, "Invalid registration payload: %s", payloadStr.c_str());
        return false;
    }

    name = parts[1];
    mac = parts[2];

    if (name.empty() || mac.empty())
    {
        ESP_LOGE(TAG, "Invalid registration payload: %s, %s", name.c_str(), mac.c_str());
        return false;
    }

    if (!AstrOsStringUtils::caseInsensitiveCmp(mac, this->mac))
    {
        ESP_LOGI(TAG, "Registraion received for different device: %s, this device is %s", mac.c_str(), this->mac.c_str());
        return false;
    }

    if (!esp_now_is_peer_exist(src))
    {
        err = this->addPeer(src);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            return false;
        }
    }

    this->name = name;
    this->updateMasterMac(src);
    this->cachePeer(src, "Master");
    this->updateSeviceConfigCallback(name, src);
    this->displayUpdateCallback("Padawan", "", name);

    this->sendRegistrationAck();

    return true;
}

bool AstrOsEspNow::sendRegistrationAck()
{
    bool result = true;

    uint8_t *destMac = (uint8_t *)malloc(ESP_NOW_ETH_ALEN);

    this->getMasterMac(destMac);

    if (IS_BROADCAST_ADDR(destMac))
    {
        ESP_LOGE(TAG, "Cannot send registration ack to broadcast address");
        free(destMac);
        return false;
    }

    astros_espnow_data_t data = AstrOsEspNowMessageService::generateEspNowMsg(AstrOsPacketType::REGISTRATION_ACK, this->name, this->mac);

    if (esp_now_send(destMac, data.data, data.size) != ESP_OK)
    {
        ESP_LOGE(TAG, "Error sending registration ack");
        result = false;
    }

    free(data.data);
    free(destMac);

    queue_svc_cmd_t cmd;
    cmd.cmd = SERVICE_COMMAND::ESPNOW_DISCOVERY_MODE_OFF;
    cmd.data = nullptr;

    if (xQueueSend(this->serviceQueue, &cmd, 100) != pdTRUE)
    {
        ESP_LOGE(TAG, "Send service queue fail");
    }

    return result;
}

bool AstrOsEspNow::handleRegistrationAck(u_int8_t *src, u_int8_t *payload, size_t len)
{
    std::string payloadStr(reinterpret_cast<char *>(payload), len);

    std::vector<std::string> parts = AstrOsStringUtils::splitString(payloadStr, UNIT_SEPARATOR);

    if (parts.size() < 3)
    {
        ESP_LOGE(TAG, "Invalid registration ack payload: %s", payloadStr.c_str());
        return false;
    }

    name = parts[1];
    mac = parts[2];

    if (name.empty() || mac.empty())
    {
        ESP_LOGE(TAG, "Invalid registration ack payload: %s, %s", name.c_str(), mac.c_str());
        return false;
    }

    std::string sourceMac = AstrOsStringUtils::macToString(src);

    if (!AstrOsStringUtils::caseInsensitiveCmp(mac, sourceMac))
    {
        ESP_LOGI(TAG, "Invalid registration ack, mac mismatch: %s != %s", mac.c_str(), sourceMac.c_str());
        return false;
    }

    return this->cachePeer(src, name);

    return true;
}

void AstrOsEspNow::pollPadawans()
{
    for (auto &peer : peers)
    {
        if (memcmp(peer.mac_addr, broadcastMac, ESP_NOW_ETH_ALEN) == 0)
        {
            continue;
        }

        peer.pollAckThisCycle = false;

        astros_espnow_data_t data = AstrOsEspNowMessageService::generateEspNowMsg(AstrOsPacketType::POLL, this->name, this->mac);

        if (esp_now_send(peer.mac_addr, data.data, data.size) != ESP_OK)
        {
            ESP_LOGE(TAG, "Error sending poll message to " MACSTR, MAC2STR(peer.mac_addr));
        }

        free(data.data);
    }
}

bool AstrOsEspNow::handlePoll(astros_packet_t packet)
{
    bool result = true;

    uint8_t *destMac = (uint8_t *)malloc(ESP_NOW_ETH_ALEN);

    this->getMasterMac(destMac);

    if (IS_BROADCAST_ADDR(destMac))
    {
        ESP_LOGE(TAG, "Cannot send poll ack to broadcast address");
        free(destMac);
        return false;
    }

    astros_espnow_data_t data = AstrOsEspNowMessageService::generateEspNowMsg(AstrOsPacketType::POLL_ACK, this->name, this->mac);

    if (esp_now_send(destMac, data.data, data.size) != ESP_OK)
    {
        ESP_LOGE(TAG, "Error sending registration ack");
        result = false;
    }

    free(data.data);
    free(destMac);

    return result;
}

bool AstrOsEspNow::handlePollAck(astros_packet_t packet)
{
    std::string payload = std::string((char *)packet.payload, packet.payloadSize);

    auto parts = AstrOsStringUtils::splitString(payload, UNIT_SEPARATOR);

    if (parts.size() < 3 || parts[0] != "POLL_ACK")
    {
        return false;
    }

    auto padawan = parts[1];
    auto fingerprint = parts[2];

    bool found = false;

    for (auto &peer : peers)
    {
        if (memcmp(peer.name, padawan.c_str(), ESP_NOW_ETH_ALEN) == 0)
        {
            peer.pollAckThisCycle = true;
            found = true;
            break;
        }
    }

    if (!found)
    {
        ESP_LOGI(TAG, "Padawan not found in peer list: %s", padawan.c_str());
        return false;
    }

    queue_svc_cmd_t cmd;

    cmd.cmd = SERVICE_COMMAND::FORWARD_TO_SERIAL;
    auto msg = AstrOsSerialMessageService::generatePollAckMsg(padawan, fingerprint);
    auto size = msg.length();

    cmd.data = (uint8_t *)malloc(size);
    memcpy(cmd.data, msg.c_str(), size);
    cmd.dataSize = size;

    if (xQueueSend(this->serviceQueue, &cmd, 100) != pdTRUE)
    {
        ESP_LOGE(TAG, "Send service queue fail");
        free(cmd.data);
        return false;
    }

    return true;
}

void AstrOsEspNow::pollRepsonseTimeExpired()
{
    for (auto &peer : peers)
    {
        if (memcmp(peer.mac_addr, broadcastMac, ESP_NOW_ETH_ALEN) == 0)
        {
            continue;
        }

        if (!peer.pollAckThisCycle)
        {
            ESP_LOGI(TAG, "Poll response time expired for %s:" MACSTR, peer.name, MAC2STR(peer.mac_addr));

            queue_svc_cmd_t cmd;

            cmd.cmd = SERVICE_COMMAND::FORWARD_TO_SERIAL;

            auto msg = AstrOsSerialMessageService::generatePollNakMsg(peer.name);
            auto size = msg.length();

            cmd.data = (uint8_t *)malloc(size);
            memcpy(cmd.data, msg.c_str(), size);
            cmd.dataSize = size;

            if (xQueueSend(this->serviceQueue, &cmd, 100) != pdTRUE)
            {
                ESP_LOGE(TAG, "Send service queue fail");
            }
        }
        else
        {
            peer.pollAckThisCycle = false;
        }
    }
}
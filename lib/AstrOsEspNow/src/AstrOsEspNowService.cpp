#include <AstrOsEspNowService.hpp>
#include <AstrOsUtility_Esp.h>
#include <AstrOsMessaging.hpp>
#include <AstrOsUtility.h>

#include <esp_err.h>
#include <esp_now.h>
#include <esp_log.h>
#include <string.h>
#include <string>
#include <sstream>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_mac.h>

#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_crc.h>
#include <esp_wifi_types.h>
#include <esp_timer.h>
#include <AstrOsInterfaceResponseMsg.hpp>

static const char *TAG = "AstrOsEspNow";
SemaphoreHandle_t masterMacMutex;
SemaphoreHandle_t valueMutex;

AstrOsEspNow AstrOs_EspNow;

static void (*espnowSendCallback)(const uint8_t *mac_addr, esp_now_send_status_t status);
static void (*espnowRecvCallback)(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len);

AstrOsEspNow::AstrOsEspNow()
{
}

AstrOsEspNow::~AstrOsEspNow()
{
}

esp_err_t AstrOsEspNow::init(astros_espnow_config_t config)
{
    ESP_LOGI(TAG, "Initializing AstrOsEspNow");

    esp_err_t err = ESP_OK;

    this->name = config.name;
    this->fingerprint = config.fingerprint;
    this->isMasterNode = config.isMaster;
    this->peers = {};
    this->serviceQueue = config.serviceQueue;
    this->interfaceQueue = config.interfaceQueue;

    espnowSendCallback = config.espnowSend_cb;
    espnowRecvCallback = config.espnowRecv_cb;
    this->cachePeerCallback = config.cachePeer_cb;
    this->displayUpdateCallback = config.displayUpdate_cb;
    this->updateSeviceConfigCallback = config.updateSeviceConfig_cb;

    err = wifiInit();
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    err = espnowInit();
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

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

    valueMutex = xSemaphoreCreateMutex();

    if (valueMutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialize the value mutex");
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

    this->packetTracker = PacketTracker();

    ESP_LOGI(TAG, "AstrOsEspNow initialized");

    return err;
}

std::string AstrOsEspNow::getMac()
{
    if (isMasterNode)
    {
        return "00:00:00:00:00:00";
    }

    std::string macAddress;

    bool macSet = false;
    while (!macSet)
    {
        if (xSemaphoreTake(valueMutex, 100 / portTICK_PERIOD_MS))
        {
            macAddress = this->mac;
            macSet = true;
            xSemaphoreGive(valueMutex);
        }
        else
        {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }

    return macAddress;
}

std::string AstrOsEspNow::getName()
{
    std::string name;

    bool nameSet = false;
    while (!nameSet)
    {
        if (xSemaphoreTake(valueMutex, 100 / portTICK_PERIOD_MS))
        {
            name = this->name;
            nameSet = true;
            xSemaphoreGive(valueMutex);
        }
        else
        {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }

    return name;
}

std::string AstrOsEspNow::getFingerprint()
{
    std::string fingerprint;

    bool fingerprintSet = false;
    while (!fingerprintSet)
    {
        if (xSemaphoreTake(valueMutex, 100 / portTICK_PERIOD_MS))
        {
            fingerprint = this->fingerprint;
            fingerprintSet = true;
            xSemaphoreGive(valueMutex);
        }
        else
        {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }

    return fingerprint;
}

void AstrOsEspNow::updateFingerprint(std::string fingerprint)
{
    bool fingerprintSet = false;
    while (!fingerprintSet)
    {
        if (xSemaphoreTake(valueMutex, 100 / portTICK_PERIOD_MS))
        {
            this->fingerprint = fingerprint;
            fingerprintSet = true;
            xSemaphoreGive(valueMutex);
        }
        else
        {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }
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
            ESP_LOGW(TAG, "Peer already cached");
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

std::vector<espnow_peer_t> AstrOsEspNow::getPeers()
{
    std::vector<espnow_peer_t> result;

    result.clear();

    espnow_peer_t master = {
        .id = 0,
        .name = "master"};

    memcpy(master.mac_addr, nullMac, ESP_NOW_ETH_ALEN);

    result.push_back(master);

    for (auto &peer : peers)
    {
        result.push_back(peer);
    }

    return result;
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
        return this->handleRegistration(src, packet);
    case AstrOsPacketType::REGISTRATION_ACK:
        if (!isMasterNode)
        {
            break;
        }
        ESP_LOGI(TAG, "Registration ack received from " MACSTR, MAC2STR(src));
        return this->handleRegistrationAck(src, packet);
    case AstrOsPacketType::POLL:
        if (isMasterNode)
        {
            break;
        }
        ESP_LOGD(TAG, "Poll received from " MACSTR ", ", MAC2STR(src));
        return this->handlePoll(packet);
    case AstrOsPacketType::POLL_ACK:
        if (!isMasterNode)
        {
            break;
        }
        ESP_LOGD(TAG, "Poll ACK received from " MACSTR ", ", MAC2STR(src));
        return this->handlePollAck(packet);
    case AstrOsPacketType::CONFIG:
        ESP_LOGI(TAG, "Config update received from " MACSTR ", ", MAC2STR(src));
        return this->handleConfig(packet);
    case AstrOsPacketType::CONFIG_ACK:
    case AstrOsPacketType::CONFIG_NAK:
        ESP_LOGI(TAG, "Config ack/nak received from " MACSTR ", ", MAC2STR(src));
        return this->handleConfigAckNak(packet);
    default:
        ESP_LOGE(TAG, "Unknown packet type received");
        return false;
    }

    return true;
}

/*******************************************
 * Registration methods
 *******************************************/

void AstrOsEspNow::sendRegistrationRequest()
{
    uint8_t *destMac = (uint8_t *)malloc(ESP_NOW_ETH_ALEN);

    getMasterMac(destMac);

    // if master mac is broadcast address, then send registration request
    if (IS_BROADCAST_ADDR(destMac))
    {
        astros_espnow_data_t data = AstrOsEspNowMessageService::generateEspNowMsg(AstrOsPacketType::REGISTRATION_REQ)[0];
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
    astros_espnow_data_t data = AstrOsEspNowMessageService::generateEspNowMsg(AstrOsPacketType::REGISTRATION, macStr, padewanName)[0];

    err = esp_now_send(broadcastMac, data.data, data.size);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return false;
    }

    free(data.data);
    return true;
}

bool AstrOsEspNow::handleRegistration(u_int8_t *src, astros_packet_t packet)
{
    esp_err_t err = ESP_OK;

    std::string payloadStr(reinterpret_cast<char *>(packet.payload), packet.payloadSize);

    std::string name;
    std::string mac;

    auto parts = AstrOsStringUtils::splitString(payloadStr, UNIT_SEPARATOR);

    if (parts.size() < 2)
    {
        ESP_LOGE(TAG, "Invalid registration payload: %s", payloadStr.c_str());
        return false;
    }

    name = parts[0];
    mac = parts[1];

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

    astros_espnow_data_t data = AstrOsEspNowMessageService::generateEspNowMsg(AstrOsPacketType::REGISTRATION_ACK, this->mac, this->name)[0];

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

bool AstrOsEspNow::handleRegistrationAck(u_int8_t *src, astros_packet_t packet)
{
    std::string payloadStr(reinterpret_cast<char *>(packet.payload), packet.payloadSize);

    std::vector<std::string> parts = AstrOsStringUtils::splitString(payloadStr, UNIT_SEPARATOR);

    if (parts.size() < 2)
    {
        ESP_LOGE(TAG, "Invalid registration ack payload: %s", payloadStr.c_str());
        return false;
    }

    name = parts[0];
    mac = parts[1];

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

/*******************************************
 * Poll methods
 *******************************************/

void AstrOsEspNow::pollPadawans()
{
    for (auto &peer : peers)
    {
        if (memcmp(peer.mac_addr, broadcastMac, ESP_NOW_ETH_ALEN) == 0)
        {
            continue;
        }

        peer.pollAckThisCycle = false;

        astros_espnow_data_t data = AstrOsEspNowMessageService::generateEspNowMsg(AstrOsPacketType::POLL, this->mac)[0];

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

    std::stringstream ss;
    ss << this->getName() << UNIT_SEPARATOR << this->getFingerprint();

    astros_espnow_data_t data = AstrOsEspNowMessageService::generateEspNowMsg(AstrOsPacketType::POLL_ACK, this->mac, ss.str())[0];

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

    if (parts.size() < 3)
    {
        return false;
    }

    auto padawanMac = parts[0];
    auto padawan = parts[1];
    auto fingerprint = parts[2];

    bool found = false;

    for (auto &peer : peers)
    {
        auto peerMac = AstrOsStringUtils::macToString(peer.mac_addr);

        if (memcmp(peerMac.c_str(), padawanMac.c_str(), padawanMac.length()) == 0)
        {
            peer.pollAckThisCycle = true;
            found = true;
            break;
        }
    }

    if (!found)
    {
        ESP_LOGW(TAG, "Padawan not found in peer list=> %s : %s", padawan.c_str(), padawanMac.c_str());
        return false;
    }

    queue_svc_cmd_t cmd;

    cmd.cmd = SERVICE_COMMAND::ASTROS_INTERFACE_MESSAGE;
    auto msg = AstrOsSerialMessageService::getPollAck(padawanMac, padawan, fingerprint);
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
            ESP_LOGD(TAG, "Poll response time expired for %s:" MACSTR, peer.name, MAC2STR(peer.mac_addr));

            queue_svc_cmd_t cmd;

            cmd.cmd = SERVICE_COMMAND::ASTROS_INTERFACE_MESSAGE;

            auto macStr = AstrOsStringUtils::macToString(peer.mac_addr);

            auto msg = AstrOsSerialMessageService::getPollNak(macStr, peer.name);
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

/*******************************************
 * Configuration methods
 *******************************************/

void AstrOsEspNow::sendConfigUpdate(std::string peer, std::string msgId, std::string msg)
{
    bool found = false;
    for (auto &p : peers)
    {
        if (memcmp(p.mac_addr, broadcastMac, ESP_NOW_ETH_ALEN) == 0)
        {
            continue;
        }

        std::string peerMac = AstrOsStringUtils::macToString(p.mac_addr);

        if (peerMac != peer)
        {
            continue;
        }

        found = true;

        uint8_t *destMac = (uint8_t *)malloc(ESP_NOW_ETH_ALEN);
        memcpy(destMac, p.mac_addr, ESP_NOW_ETH_ALEN);

        std::stringstream ss;
        ss << msgId << UNIT_SEPARATOR << msg;

        ESP_LOGI(TAG, "Sending config update to " MACSTR, MAC2STR(destMac));

        auto data = AstrOsEspNowMessageService::generateEspNowMsg(AstrOsPacketType::CONFIG, peer, ss.str());

        for (auto &packet : data)
        {
            if (esp_now_send(destMac, packet.data, packet.size) != ESP_OK)
            {
                ESP_LOGE(TAG, "Error sending config update to " MACSTR, MAC2STR(destMac));
            }

            free(packet.data);
        }

        free(destMac);
    }

    if (!found)
    {
        ESP_LOGW(TAG, "Peer not found in peer for config send: %s", peer.c_str());
    }
}

bool AstrOsEspNow::handleConfig(astros_packet_t packet)
{

    std::string payload;

    if (packet.totalPackets > 1)
    {
        payload = this->handleMultiPacketMessage(packet);
        if (payload.empty())
        {
            return true;
        }
    }
    else
    {
        payload = std::string((char *)packet.payload, packet.payloadSize);
    }

    // 0 is dest mac, 1 is orgination msg id, 2 is message
    auto parts = AstrOsStringUtils::splitString(payload, UNIT_SEPARATOR);
    auto idSize = parts[1].size() + 1;
    auto configSize = parts[2].size() + 1;

    astros_interface_response_t response = {
        .type = AstrOsInterfaceResponseType::SET_CONFIG,
        .originationMsgId = (char *)malloc(idSize),
        .peerMac = nullptr,
        .peerName = nullptr,
        .message = (char *)malloc(configSize)};

    memcpy(response.originationMsgId, parts[1].c_str(), idSize);
    memcpy(response.message, parts[2].c_str(), configSize);

    if (xQueueSend(this->interfaceQueue, &response, pdTICKS_TO_MS(250)) == pdFALSE)
    {
        ESP_LOGE(TAG, "Failed to send message to interface handler queue");
        free(response.originationMsgId);
        free(response.message);
    }

    return true;
}

void AstrOsEspNow::sendConfigAckNak(std::string msgId, bool success)
{
    uint8_t *destMac = (uint8_t *)malloc(ESP_NOW_ETH_ALEN);

    this->getMasterMac(destMac);

    std::stringstream ss;
    ss << this->getName() << UNIT_SEPARATOR << msgId << UNIT_SEPARATOR << this->getFingerprint();

    auto ackNak = success ? AstrOsPacketType::CONFIG_ACK : AstrOsPacketType::CONFIG_NAK;

    auto packet = AstrOsEspNowMessageService::generateEspNowMsg(ackNak, this->getMac(), ss.str())[0];

    if (esp_now_send(destMac, packet.data, packet.size) != ESP_OK)
    {
        ESP_LOGE(TAG, "Error sending config update to " MACSTR, MAC2STR(destMac));
    }

    free(packet.data);
    free(destMac);
}

bool AstrOsEspNow::handleConfigAckNak(astros_packet_t packet)
{
    auto payload = std::string((char *)packet.payload, packet.payloadSize);

    ESP_LOGD(TAG, "Config ack/nak received: %s", payload.c_str());

    auto parts = AstrOsStringUtils::splitString(payload, UNIT_SEPARATOR);

    if (parts.size() < 4)
    {
        ESP_LOGE(TAG, "Invalid config ack/nak payload: %s", payload.c_str());
        return false;
    }

    auto responseType = packet.packetType == AstrOsPacketType::CONFIG_ACK ? AstrOsInterfaceResponseType::SEND_CONFIG_ACK : AstrOsInterfaceResponseType::SEND_CONFIG_NAK;

    astros_interface_response_t response = {
        .type = responseType,
        .originationMsgId = (char *)malloc(parts[2].size() + 1),
        .peerMac = (char *)malloc(parts[0].size() + 1),
        .peerName = (char *)malloc(parts[1].size() + 1),
        .message = (char *)malloc(parts[3].size() + 1)};

    memcpy(response.originationMsgId, parts[2].c_str(), parts[2].size() + 1);
    memcpy(response.peerMac, parts[0].c_str(), parts[0].size() + 1);
    memcpy(response.peerName, parts[1].c_str(), parts[2].size() + 1);
    memcpy(response.message, parts[3].c_str(), parts[3].size() + 1);

    if (xQueueSend(this->interfaceQueue, &response, pdTICKS_TO_MS(250)) == pdFALSE)
    {
        ESP_LOGE(TAG, "Failed to send message to interface handler queue");
        free(response.originationMsgId);
        free(response.peerMac);
        free(response.peerName);
        free(response.message);
    }

    return true;
}

/*******************************************
 * Common Methods
 *******************************************/

/// @brief adds the payload to the packet tracker. When getting the
/// @param packet
/// @return if the message is complete, it returns the message, otherwise it returns an empty string.
std::string AstrOsEspNow::handleMultiPacketMessage(astros_packet_t packet)
{
    auto msgId = std::string((char *)packet.id, 16);
    auto payload = std::string((char *)packet.payload, packet.payloadSize);

    PacketData packetData = {
        .packetNumber = packet.packetNumber,
        .totalPackets = packet.totalPackets,
        .payload = payload};

    auto result = this->packetTracker.addPacket(msgId, packetData, esp_timer_get_time() / 1000);

    ESP_LOGD(TAG, "Multi packet message added to tracker with result: %d", result);

    if (result == AddPacketResult::MESSAGE_COMPLETE)
    {
        return this->packetTracker.getMessage(msgId);
    }

    return "";
}

/**********************************
 * Hardware Initialization
 ***********************************/

esp_err_t AstrOsEspNow::wifiInit(void)
{

    ESP_LOGI(TAG, "wifiInit called");

    esp_err_t err = ESP_OK;

    err = esp_netif_init();
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    err = esp_event_loop_create_default();
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    err = esp_wifi_init(&cfg);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    err = esp_wifi_set_mode(ESPNOW_WIFI_MODE);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    err = esp_wifi_start();
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    err = esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

    err = esp_wifi_set_protocol(ESPNOW_WIFI_IF, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    ESP_LOGI(TAG, "wifiInit complete");

    return err;
}

esp_err_t AstrOsEspNow::espnowInit(void)
{

    ESP_LOGI(TAG, "espnowInit called");

    esp_err_t err = ESP_OK;

    err = esp_now_init();
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    err = esp_now_register_send_cb(espnowSendCallback);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    err = esp_now_register_recv_cb(espnowRecvCallback);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    err = esp_now_set_pmk((uint8_t *)CONFIG_ESPNOW_PMK);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    ESP_LOGI(TAG, "espnowInit complete");

    return err;
}
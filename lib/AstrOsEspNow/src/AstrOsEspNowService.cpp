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

    this->messageService = AstrOsEspNowMessageService();

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

/// @brief Handle incoming messages from espnow
/// @param src
/// @param data
/// @param len
/// @return
bool AstrOsEspNow::handleMessage(u_int8_t *src, u_int8_t *data, size_t len)
{
    astros_packet_t packet = this->messageService.parsePacket(data);

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
        return this->handleRegistration(src, packet);
    case AstrOsPacketType::REGISTRATION_ACK:
        if (!isMasterNode)
        {
            break;
        }
        return this->handleRegistrationAck(src, packet);
    case AstrOsPacketType::POLL:
        if (isMasterNode)
        {
            break;
        }
        return this->handlePoll(packet);
    case AstrOsPacketType::POLL_ACK:
        if (!isMasterNode)
        {
            break;
        }
        return this->handlePollAck(packet);
    case AstrOsPacketType::CONFIG:
        return this->handleConfig(packet);
    case AstrOsPacketType::CONFIG_ACK:
    case AstrOsPacketType::CONFIG_NAK:
        return this->handleConfigAckNak(packet);
    case AstrOsPacketType::SCRIPT_DEPLOY:
        return this->handleScriptDeploy(packet);
    case AstrOsPacketType::SCRIPT_RUN:
        return this->handleScriptRun(packet);
    case AstrOsPacketType::PANIC_STOP:
        return this->handlePanicStop(packet);
    case AstrOsPacketType::FORMAT_SD:
        return this->handleFormatSD(packet);
    case AstrOsPacketType::SCRIPT_DEPLOY_ACK:
    case AstrOsPacketType::SCRIPT_DEPLOY_NAK:
    case AstrOsPacketType::SCRIPT_RUN_ACK:
    case AstrOsPacketType::SCRIPT_RUN_NAK:
    case AstrOsPacketType::FORMAT_SD_ACK:
    case AstrOsPacketType::FORMAT_SD_NAK:
        ESP_LOGD(TAG, "Basic ack/nak received for type %d from " MACSTR, (int)packet.packetType, MAC2STR(src));
        return this->handleBasicAckNak(packet);
    default:
        ESP_LOGE(TAG, "Unknown packet type received");
        auto test = std::string((char *)data, len);
        ESP_LOGI(TAG, "packet: %s", test.c_str());
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
        astros_espnow_data_t data = this->messageService.generateEspNowMsg(AstrOsPacketType::REGISTRATION_REQ)[0];
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
    astros_espnow_data_t data = this->messageService.generateEspNowMsg(AstrOsPacketType::REGISTRATION, macStr, padewanName)[0];

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

    astros_espnow_data_t data = this->messageService.generateEspNowMsg(AstrOsPacketType::REGISTRATION_ACK, this->mac, this->name)[0];

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

        astros_espnow_data_t data = this->messageService.generateEspNowMsg(AstrOsPacketType::POLL, this->mac)[0];

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

    astros_espnow_data_t data = this->messageService.generateEspNowMsg(AstrOsPacketType::POLL_ACK, this->mac, ss.str())[0];

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

    if (!this->isValidPollPeer(padawanMac))
    {
        ESP_LOGW(TAG, "Padawan not found in peer list=> %s : %s", padawan.c_str(), padawanMac.c_str());
        return false;
    }

    this->sendToInterfaceQueue(AstrOsInterfaceResponseType::SEND_POLL_ACK,
                               "", padawanMac, padawan, fingerprint);

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

            this->sendToInterfaceQueue(AstrOsInterfaceResponseType::SEND_POLL_NAK,
                                       "", macStr, peer.name, "");
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

    if (parts.size() < 3)
    {
        ESP_LOGE(TAG, "Invalid config payload: %s", payload.c_str());
        return false;
    }

    this->sendToInterfaceQueue(AstrOsInterfaceResponseType::SET_CONFIG,
                               parts[1], "", "", parts[2]);

    return true;
}

void AstrOsEspNow::sendConfigAckNak(std::string msgId, bool success)
{
    uint8_t *destMac = (uint8_t *)malloc(ESP_NOW_ETH_ALEN);

    this->getMasterMac(destMac);

    std::stringstream ss;
    ss << msgId << UNIT_SEPARATOR << this->getName() << UNIT_SEPARATOR << this->getFingerprint();

    auto ackNak = success ? AstrOsPacketType::CONFIG_ACK : AstrOsPacketType::CONFIG_NAK;

    auto packet = this->messageService.generateEspNowMsg(ackNak, this->getMac(), ss.str())[0];

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

    auto responseType = this->getInterfaceResponseType(packet.packetType);

    this->sendToInterfaceQueue(responseType, parts[1], parts[0], parts[2], parts[3]);

    return true;
}

/*******************************************
 * Script Deployment methods
 *******************************************/

bool AstrOsEspNow::handleScriptDeploy(astros_packet_t packet)
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

    // 0 is dest mac, 1 is orgination msg id, 2 is scriptId, 3 is script
    auto parts = AstrOsStringUtils::splitString(payload, UNIT_SEPARATOR);

    if (parts.size() < 4)
    {
        ESP_LOGE(TAG, "Invalid script deploy payload: %s", payload.c_str());
        return false;
    }

    std::stringstream ss;
    ss << parts[2] << UNIT_SEPARATOR << parts[3];

    this->sendToInterfaceQueue(AstrOsInterfaceResponseType::SAVE_SCRIPT,
                               parts[1], "", "", ss.str());

    return true;
}

/*******************************************
 * Script Run methods
 *******************************************/

bool AstrOsEspNow::handleScriptRun(astros_packet_t packet)
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

    // 0 is dest mac, 1 is orgination msg id, 2 is scriptId
    auto parts = AstrOsStringUtils::splitString(payload, UNIT_SEPARATOR);

    if (parts.size() < 3)
    {
        ESP_LOGE(TAG, "Invalid script run payload: %s", payload.c_str());
        return false;
    }

    this->sendToInterfaceQueue(AstrOsInterfaceResponseType::SCRIPT_RUN,
                               parts[1], "", "", parts[2]);

    return true;
}

bool AstrOsEspNow::handlePanicStop(astros_packet_t packet)
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

    // 0 is dest mac, 1 is orgination msg id
    auto parts = AstrOsStringUtils::splitString(payload, UNIT_SEPARATOR);

    if (parts.size() < 3)
    {
        ESP_LOGE(TAG, "Invalid panic stop payload: %s", payload.c_str());
        return false;
    }

    this->sendToInterfaceQueue(AstrOsInterfaceResponseType::PANIC_STOP,
                               parts[1], "", "", "");

    return true;
}
/*******************************************
 * Utility Methods
 *******************************************/

bool AstrOsEspNow::handleFormatSD(astros_packet_t packet)
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

    // 0 is dest mac, 1 is orgination msg id
    auto parts = AstrOsStringUtils::splitString(payload, UNIT_SEPARATOR);

    if (parts.size() < 3)
    {
        ESP_LOGE(TAG, "Invalid format sd payload: %s", payload.c_str());
        return false;
    }

    this->sendToInterfaceQueue(AstrOsInterfaceResponseType::FORMAT_SD,
                               parts[1], "", "", "");

    return true;
}

/*******************************************
 * Common Methods
 *******************************************/

/// @brief send a basic espnow message to the provided peer.
/// @param type
/// @param peer
/// @param msgId
/// @param msg
void AstrOsEspNow::sendBasicCommand(AstrOsPacketType type, std::string peer, std::string msgId, std::string msg)
{
    if (!this->findPeer(peer))
    {
        ESP_LOGW(TAG, "Peer not found in peer list for script run send: %s", peer.c_str());
        return;
    }

    std::stringstream ss;
    ss << msgId << UNIT_SEPARATOR << msg;

    ESP_LOGI(TAG, "Sending script run to %s for type %d", peer.c_str(), (int)type);

    this->sendEspNowMessage(type, peer, ss.str());
}

/// @brief Sends a basic ack or nak to the master node for the provided packet type.
/// @param msgId
/// @param type
void AstrOsEspNow::sendBasicAckNak(std::string msgId, AstrOsPacketType type)
{
    uint8_t *destMac = (uint8_t *)malloc(ESP_NOW_ETH_ALEN);

    this->getMasterMac(destMac);

    std::stringstream ss;
    ss << this->getName() << UNIT_SEPARATOR << msgId;

    auto packet = this->messageService.generateEspNowMsg(type, this->getMac(), ss.str())[0];

    if (esp_now_send(destMac, packet.data, packet.size) != ESP_OK)
    {
        ESP_LOGE(TAG, "Error sending basic ack/nak for type %d to " MACSTR, (int)type, MAC2STR(destMac));
    }

    free(packet.data);
    free(destMac);
}

/// @brief Handles a basic ack or nak packet and sends the response to the interface handler queue.
/// @param packet
/// @return
bool AstrOsEspNow::handleBasicAckNak(astros_packet_t packet)
{
    auto payload = std::string((char *)packet.payload, packet.payloadSize);

    ESP_LOGD(TAG, "Basic ack/nak received: %s", payload.c_str());

    auto parts = AstrOsStringUtils::splitString(payload, UNIT_SEPARATOR);

    if (parts.size() < 3)
    {
        ESP_LOGE(TAG, "Invalid basic ack/nak payload: %s", payload.c_str());
        return false;
    }

    auto responseType = this->getInterfaceResponseType(packet.packetType);

    this->sendToInterfaceQueue(responseType, parts[0], parts[1], parts[2], "");

    return true;
}

/// @brief gets the response type for the provided packet type.
/// @param type
/// @return
AstrOsInterfaceResponseType AstrOsEspNow::getInterfaceResponseType(AstrOsPacketType type)
{
    switch (type)
    {
    case AstrOsPacketType::CONFIG_ACK:
        return AstrOsInterfaceResponseType::SEND_CONFIG_ACK;
    case AstrOsPacketType::CONFIG_NAK:
        return AstrOsInterfaceResponseType::SEND_CONFIG_NAK;
    case AstrOsPacketType::SCRIPT_DEPLOY_ACK:
        return AstrOsInterfaceResponseType::SAVE_SCRIPT_ACK;
    case AstrOsPacketType::SCRIPT_DEPLOY_NAK:
        return AstrOsInterfaceResponseType::SAVE_SCRIPT_NAK;
    case AstrOsPacketType::SCRIPT_RUN_ACK:
        return AstrOsInterfaceResponseType::SCRIPT_RUN_ACK;
    case AstrOsPacketType::SCRIPT_RUN_NAK:
        return AstrOsInterfaceResponseType::SCRIPT_RUN_NAK;
    case AstrOsPacketType::FORMAT_SD_ACK:
        return AstrOsInterfaceResponseType::FORMAT_SD_ACK;
    case AstrOsPacketType::FORMAT_SD_NAK:
        return AstrOsInterfaceResponseType::FORMAT_SD_NAK;
    default:
        return AstrOsInterfaceResponseType::UNKNOWN;
    }
}

/// @brief adds the payload to the packet tracker.
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

/// @brief finds the peer in the peer list and sets the pollAckThisCycle flag if pollAck is true.
/// @param peerMac
/// @param pollAck
/// @return
bool AstrOsEspNow::findPeer(std::string peerMac)
{
    for (auto &p : peers)
    {
        auto pMac = AstrOsStringUtils::macToString(p.mac_addr);

        if (memcmp(pMac.c_str(), peerMac.c_str(), peerMac.size()) == 0)
        {
            return true;
        }
    }

    return false;
}

bool AstrOsEspNow::isValidPollPeer(std::string peerMac)
{
    for (auto &p : peers)
    {
        auto pMac = AstrOsStringUtils::macToString(p.mac_addr);

        if (memcmp(pMac.c_str(), peerMac.c_str(), ESP_NOW_ETH_ALEN) == 0)
        {
            p.pollAckThisCycle = true;
            return true;
        }
    }

    return false;
}

/// @brief sends an esp now message to the provided peer.
/// @param type
/// @param peer
/// @param msg
void AstrOsEspNow::sendEspNowMessage(AstrOsPacketType type, std::string peer, std::string msg)
{
    auto peerMac = AstrOsStringUtils::stringToMac(peer);
    uint8_t *destMac = (uint8_t *)malloc(ESP_NOW_ETH_ALEN);
    memcpy(destMac, peerMac, ESP_NOW_ETH_ALEN);

    auto data = this->messageService.generateEspNowMsg(type, peer, msg);

    for (auto &packet : data)
    {
        if (esp_now_send(destMac, packet.data, packet.size) != ESP_OK)
        {
            ESP_LOGE(TAG, "Error sending packet type %d to " MACSTR, (int)type, MAC2STR(destMac));
        }

        free(packet.data);
    }

    free(destMac);
}

void AstrOsEspNow::sendToInterfaceQueue(AstrOsInterfaceResponseType responseType, std::string msgId, std::string peerMac, std::string peerName, std::string message)
{
    auto msgIdSize = msgId.size() + 1;
    auto peerMacSize = peerMac.size() + 1;
    auto peerNameSize = peerName.size() + 1;
    auto messageSize = message.size() + 1;

    astros_interface_response_t response;
    response.type = responseType;
    response.originationMsgId = msgIdSize == 1 ? nullptr : (char *)malloc(msgIdSize);
    response.peerMac = peerMacSize == 1 ? nullptr : (char *)malloc(peerMacSize);
    response.peerName = peerNameSize == 1 ? nullptr : (char *)malloc(peerNameSize);
    response.message = messageSize == 1 ? nullptr : (char *)malloc(messageSize);

    if (msgIdSize > 1)
    {
        memcpy(response.originationMsgId, msgId.c_str(), msgIdSize);
    }
    if (peerMacSize > 1)
    {
        memcpy(response.peerMac, peerMac.c_str(), peerMacSize);
    }
    if (peerNameSize > 1)
    {
        memcpy(response.peerName, peerName.c_str(), peerNameSize);
    }
    if (messageSize > 1)
    {
        memcpy(response.message, message.c_str(), messageSize);
    }

    if (xQueueSend(this->interfaceQueue, &response, pdTICKS_TO_MS(250)) == pdFALSE)
    {
        ESP_LOGE(TAG, "Failed to send message to interface handler queue");
        free(response.originationMsgId);
        free(response.peerMac);
        free(response.peerName);
        free(response.message);
    }
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
#include <AstrOsEspNowProtocol.hpp>
#include <AstrOsEspNowService.hpp>
#include <AstrOsMessaging.hpp>
#include <AstrOsUtility.h>
#include <AstrOsUtility_ESP.h>

#include <algorithm>
#include <array>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_now.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <sstream>
#include <string.h>
#include <string>

#include <AstrOsInterfaceResponseMsg.hpp>
#include <esp_crc.h>
#include <esp_netif.h>
#include <esp_now.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>

static const char *TAG = "AstrOsEspNow";
SemaphoreHandle_t masterMacMutex;
SemaphoreHandle_t valueMutex;

AstrOsEspNow AstrOs_EspNow;

static void (*espnowSendCallback)(const esp_now_send_info_t *tx_info, esp_now_send_status_t status);
static void (*espnowRecvCallback)(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len);

AstrOsEspNow::AstrOsEspNow() {}

AstrOsEspNow::~AstrOsEspNow() {}

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

    this->peersMutex = xSemaphoreCreateMutex();

    if (this->peersMutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialize the peers mutex");
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

        if (xSemaphoreTake(this->peersMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
        {
            ESP_LOGE(TAG, "init: failed to acquire peersMutex within 1s");
            return ESP_FAIL;
        }

        auto addResult = peers.add(peer);
        auto peerCount = peers.size();
        xSemaphoreGive(this->peersMutex);

        switch (addResult)
        {
        case AstrOsEspNowPeers::AddResult::Added:
            break;
        case AstrOsEspNowPeers::AddResult::AlreadyExists:
            // NVS yielded a duplicate entry. esp_now_add_peer would normally
            // have rejected this above, so reaching here suggests a stored
            // duplicate rather than a logic bug — tolerate it but warn so it
            // shows up during QA.
            ESP_LOGW(TAG, "init: NVS peer %d (%s) already present in PeerList — skipping duplicate", i,
                     AstrOsStringUtils::macToString(peer.mac_addr).c_str());
            break;
        case AstrOsEspNowPeers::AddResult::Full:
            // PeerList capacity is lower than the NVS-stored peer count. The
            // ESP-NOW driver side has been updated for every peer so far
            // (addPeer was already called above), which would leave the
            // driver and PeerList permanently out of sync — fail init so the
            // inconsistency is obvious instead of silently breaking polling
            // and send lookups for the dropped peers.
            ESP_LOGE(TAG, "init: PeerList full at %u; cannot load NVS peer %d — failing init to avoid divergence",
                     static_cast<unsigned>(peerCount), i);
            return ESP_FAIL;
        }
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

    if (xSemaphoreTake(valueMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "getMac: failed to acquire valueMutex within 1s, returning empty");
        return "";
    }

    std::string macAddress = this->mac;
    xSemaphoreGive(valueMutex);
    return macAddress;
}

std::string AstrOsEspNow::getName()
{
    if (xSemaphoreTake(valueMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "getName: failed to acquire valueMutex within 1s, returning empty");
        return "";
    }

    std::string name = this->name;
    xSemaphoreGive(valueMutex);
    return name;
}

std::string AstrOsEspNow::getFingerprint()
{
    if (xSemaphoreTake(valueMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "getFingerprint: failed to acquire valueMutex within 1s, returning empty");
        return "";
    }

    std::string fingerprint = this->fingerprint;
    xSemaphoreGive(valueMutex);
    return fingerprint;
}

void AstrOsEspNow::updateFingerprint(std::string fingerprint)
{
    if (xSemaphoreTake(valueMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "updateFingerprint: failed to acquire valueMutex within 1s — fingerprint not updated");
        return;
    }

    this->fingerprint = fingerprint;
    xSemaphoreGive(valueMutex);
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

bool AstrOsEspNow::cachePeer(uint8_t *macAddress, std::string name)
{
    // Populate the new peer struct before taking the lock so we hold it
    // as briefly as possible.
    espnow_peer_t newPeer{};
    size_t nameLen = std::min(name.length(), (size_t)15);
    memcpy(newPeer.name, name.c_str(), nameLen);
    newPeer.name[nameLen] = '\0';
    memcpy(newPeer.mac_addr, macAddress, ESP_NOW_ETH_ALEN);
    memset(newPeer.crypto_key, 0, ESP_NOW_KEY_LEN);
    newPeer.is_paired = true;

    if (xSemaphoreTake(this->peersMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "cachePeer: failed to acquire peersMutex within 1s");
        return false;
    }

    // peer id is 0 indexed (legacy field; written for back-compat with NVS
    // reads even though nothing reads it today).
    newPeer.id = peers.size();
    auto result = peers.add(newPeer);

    xSemaphoreGive(this->peersMutex);

    switch (result)
    {
    case AstrOsEspNowPeers::AddResult::Full:
        ESP_LOGE(TAG, "Peer cache is full");
        return false;
    case AstrOsEspNowPeers::AddResult::AlreadyExists:
        ESP_LOGW(TAG, "Peer already cached");
        return true;
    case AstrOsEspNowPeers::AddResult::Added:
        // NOTE: cachePeerCallback is invoked OUTSIDE the lock to avoid lock-order
        // inversion with the storage-manager's internal synchronization — calling
        // it under peersMutex could deadlock if another task holds the storage
        // mutex and tries to take peersMutex in the opposite order.
        return this->cachePeerCallback(newPeer);
    }
    return false;
}

std::vector<espnow_peer_t> AstrOsEspNow::getPeers()
{
    std::vector<espnow_peer_t> result;

    espnow_peer_t master = {.id = 0, .name = "master"};
    memcpy(master.mac_addr, nullMac, ESP_NOW_ETH_ALEN);
    result.push_back(master);

    if (xSemaphoreTake(this->peersMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "getPeers: failed to acquire peersMutex within 1s");
        return result;
    }

    auto snapshot = peers.all();
    xSemaphoreGive(this->peersMutex);

    for (auto &peer : snapshot)
    {
        result.push_back(peer);
    }
    return result;
}

void AstrOsEspNow::updateMasterMac(uint8_t *macAddress)
{
    if (xSemaphoreTake(masterMacMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "updateMasterMac: failed to acquire masterMacMutex within 1s — master MAC not updated");
        return;
    }

    memcpy(this->masterMac, macAddress, ESP_NOW_ETH_ALEN);
    xSemaphoreGive(masterMacMutex);
}

void AstrOsEspNow::getMasterMac(uint8_t *macAddress)
{
    if (xSemaphoreTake(masterMacMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "getMasterMac: failed to acquire masterMacMutex within 1s — zeroing output buffer");
        memset(macAddress, 0, ESP_NOW_ETH_ALEN);
        return;
    }

    memcpy(macAddress, this->masterMac, ESP_NOW_ETH_ALEN);
    xSemaphoreGive(masterMacMutex);
}

/// @brief Handle incoming messages from espnow
/// @param src
/// @param data
/// @param len
/// @return
bool AstrOsEspNow::handleMessage(uint8_t *src, uint8_t *data, size_t len)
{
    astros_packet_t packet = this->messageService.parsePacket(data);

    auto result = AstrOsEspNowProtocol::handlePacket(packet, this->packetTracker, this->isMasterNode,
                                                     esp_timer_get_time() / 1000);

    switch (result.status)
    {
    case AstrOsEspNowProtocol::HandlerStatus::Ok:
    {
        auto &msg = *result.message;
        this->sendToInterfaceQueue(msg.responseType, msg.msgId, msg.peerMac, msg.peerName, msg.message);
        return true;
    }
    case AstrOsEspNowProtocol::HandlerStatus::Pending:
        return true;
    case AstrOsEspNowProtocol::HandlerStatus::InvalidPayload:
        ESP_LOGE(TAG, "%s", result.diagnostic.c_str());
        return false;
    case AstrOsEspNowProtocol::HandlerStatus::WrongRole:
        ESP_LOGD(TAG, "Dropping packet type %d destined for the other role", (int)packet.packetType);
        return true;
    case AstrOsEspNowProtocol::HandlerStatus::UnknownType:
    {
        ESP_LOGE(TAG, "Unknown packet type received");
        auto preview = std::string((char *)data, len);
        ESP_LOGI(TAG, "packet: %s", preview.c_str());
        return false;
    }
    case AstrOsEspNowProtocol::HandlerStatus::UnsupportedType:
        // Phase 2 handlers — fall through to residual switch below.
        break;
    }

    switch (packet.packetType)
    {
    case AstrOsPacketType::REGISTRATION_REQ:
        return this->handleRegistrationReq(src);
    case AstrOsPacketType::REGISTRATION:
        return this->handleRegistration(src, packet);
    case AstrOsPacketType::REGISTRATION_ACK:
        return this->handleRegistrationAck(src, packet);
    case AstrOsPacketType::POLL:
        return this->handlePoll(packet);
    case AstrOsPacketType::POLL_ACK:
        return this->handlePollAck(packet);
    default:
        ESP_LOGE(TAG, "Dispatcher returned UnsupportedType for packet type %d but no residual handler exists",
                 (int)packet.packetType);
        return false;
    }
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

bool AstrOsEspNow::handleRegistrationReq(uint8_t *src)
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

    std::string padawanName;

    if (xSemaphoreTake(this->peersMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "handleRegistrationReq: failed to acquire peersMutex within 1s");
        return false;
    }

    // if the mac is already in the peer cache, use the existing name;
    // otherwise assign a new name based on the current peer count.
    auto existing = this->peers.findByMac(macStr);
    if (existing.has_value())
    {
        padawanName = existing->name;
    }
    else
    {
        switch (this->peers.size())
        {
        case 0:
            padawanName = "Ashoka";
            break;
        case 1:
            padawanName = "Grogu";
            break;
        case 2:
            padawanName = "Anakin";
            break;
        case 3:
            padawanName = "Obi-Wan";
            break;
        default:
            break;
        }
    }

    xSemaphoreGive(this->peersMutex);

    astros_espnow_data_t data =
        this->messageService.generateEspNowMsg(AstrOsPacketType::REGISTRATION, macStr, padawanName)[0];

    err = esp_now_send(broadcastMac, data.data, data.size);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return false;
    }

    free(data.data);
    return true;
}

bool AstrOsEspNow::handleRegistration(uint8_t *src, astros_packet_t packet)
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

    mac = parts[0];
    name = parts[1];

    if (name.empty() || mac.empty())
    {
        ESP_LOGE(TAG, "Invalid registration payload: %s, %s", name.c_str(), mac.c_str());
        return false;
    }

    if (!AstrOsStringUtils::caseInsensitiveCmp(mac, this->mac))
    {
        ESP_LOGI(TAG, "Registraion received for different device: %s, this device is %s", mac.c_str(),
                 this->mac.c_str());
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

    astros_espnow_data_t data =
        this->messageService.generateEspNowMsg(AstrOsPacketType::REGISTRATION_ACK, this->mac, this->name)[0];

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

bool AstrOsEspNow::handleRegistrationAck(uint8_t *src, astros_packet_t packet)
{
    std::string payloadStr(reinterpret_cast<char *>(packet.payload), packet.payloadSize);

    std::vector<std::string> parts = AstrOsStringUtils::splitString(payloadStr, UNIT_SEPARATOR);

    if (parts.size() < 2)
    {
        ESP_LOGE(TAG, "Invalid registration ack payload: %s", payloadStr.c_str());
        return false;
    }

    mac = parts[0];
    name = parts[1];

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
    // Reset pollAck flags + snapshot the peer list under the lock, then
    // call esp_now_send outside to avoid holding peersMutex across the
    // driver call (keeping lock scope minimal is safer for priority and
    // latency).
    if (xSemaphoreTake(this->peersMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "pollPadawans: failed to acquire peersMutex within 1s");
        return;
    }

    peers.resetPollCycle();
    auto snapshot = peers.all();
    xSemaphoreGive(this->peersMutex);

    for (auto &peer : snapshot)
    {
        if (memcmp(peer.mac_addr, broadcastMac, ESP_NOW_ETH_ALEN) == 0)
        {
            continue;
        }

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
    ss << this->getName() << UNIT_SEPARATOR << this->getFingerprint() << UNIT_SEPARATOR << AstrOsConstants::Version;

    astros_espnow_data_t data =
        this->messageService.generateEspNowMsg(AstrOsPacketType::POLL_ACK, this->mac, ss.str())[0];

    if (esp_now_send(destMac, data.data, data.size) != ESP_OK)
    {
        ESP_LOGE(TAG, "Error sending poll ack");
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
    // Phase 1+ peers append their firmware version as a 4th field. Legacy peers
    // omit it; we forward an empty string so the server records the peer's version
    // as unknown (which it then treats as incompatible — accurate semantic).
    auto peerVersion = parts.size() >= 4 ? parts[3] : std::string();

    if (xSemaphoreTake(this->peersMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "handlePollAck: failed to acquire peersMutex within 1s");
        return false;
    }
    bool known = this->peers.markPollAckReceived(padawanMac);
    xSemaphoreGive(this->peersMutex);

    if (!known)
    {
        ESP_LOGW(TAG, "Padawan not found in peer list=> %s : %s", padawan.c_str(), padawanMac.c_str());
        return false;
    }

    // Pack `fingerprint<US>version` (or just `fingerprint` for legacy peers) into
    // the interface queue's `message` field. The dispatcher in main.cpp unpacks it
    // and threads the version through to getPollAck so the version reported to
    // the server matches the peer identifiers, not the local controller's version.
    auto packed = peerVersion.empty() ? fingerprint : fingerprint + UNIT_SEPARATOR + peerVersion;
    this->sendToInterfaceQueue(AstrOsInterfaceResponseType::SEND_POLL_ACK, "", padawanMac, padawan, packed);

    return true;
}

void AstrOsEspNow::pollRepsonseTimeExpired()
{
    if (xSemaphoreTake(this->peersMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "pollRepsonseTimeExpired: failed to acquire peersMutex within 1s");
        return;
    }

    auto unacked = peers.listUnacked();
    peers.resetPollCycle();
    xSemaphoreGive(this->peersMutex);

    for (auto &peer : unacked)
    {
        if (memcmp(peer.mac_addr, broadcastMac, ESP_NOW_ETH_ALEN) == 0)
        {
            continue;
        }

        ESP_LOGD(TAG, "Poll response time expired for %s:" MACSTR, peer.name, MAC2STR(peer.mac_addr));

        auto macStr = AstrOsStringUtils::macToString(peer.mac_addr);

        this->sendToInterfaceQueue(AstrOsInterfaceResponseType::SEND_POLL_NAK, "", macStr, peer.name, "");
    }
}

/*******************************************
 * Configuration methods
 *******************************************/

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
        ESP_LOGW(TAG, "Peer not found in peer list for command %d to: %s", (int)type, peer.c_str());
        return;
    }

    std::stringstream ss;
    ss << msgId << UNIT_SEPARATOR << msg;

    ESP_LOGI(TAG, "Sending command run to %s for type %d", peer.c_str(), (int)type);

    this->sendEspNowMessage(type, peer, ss.str());
}

/// @brief Sends a basic ack or nak to the master node for the provided packet type.
/// @param msgId
/// @param type
void AstrOsEspNow::sendBasicAckNak(std::string msgId, AstrOsPacketType type, std::string msg)
{
    uint8_t *destMac = (uint8_t *)malloc(ESP_NOW_ETH_ALEN);

    this->getMasterMac(destMac);

    std::stringstream ss;
    ss << this->getName() << UNIT_SEPARATOR << msgId << UNIT_SEPARATOR << msg;

    auto packet = this->messageService.generateEspNowMsg(type, this->getMac(), ss.str())[0];

    if (esp_now_send(destMac, packet.data, packet.size) != ESP_OK)
    {
        ESP_LOGE(TAG, "Error sending basic ack/nak for type %d to " MACSTR, (int)type, MAC2STR(destMac));
    }

    free(packet.data);
    free(destMac);
}

/// @brief checks whether the given MAC (canonical "AA:BB:..." string) is in the peer list.
/// @param peerMac
/// @return
bool AstrOsEspNow::findPeer(std::string peerMac)
{
    if (xSemaphoreTake(this->peersMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "findPeer: failed to acquire peersMutex within 1s");
        return false;
    }

    bool found = this->peers.contains(peerMac);
    xSemaphoreGive(this->peersMutex);
    return found;
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

void AstrOsEspNow::sendToInterfaceQueue(AstrOsInterfaceResponseType responseType, std::string msgId,
                                        std::string peerMac, std::string peerName, std::string message)
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

    err = esp_wifi_set_protocol(ESPNOW_WIFI_IF,
                                WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
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
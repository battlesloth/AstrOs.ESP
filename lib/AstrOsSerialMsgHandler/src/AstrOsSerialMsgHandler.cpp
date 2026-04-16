
#include <AstrOsInterfaceResponseMsg.hpp>
#include <AstrOsMessaging.hpp>
#include <AstrOsSerialMsgHandler.hpp>
#include <AstrOsSerialProtocol.hpp>
#include <AstrOsUtility.h>
#include <esp_log.h>
#include <string.h>

const char *TAG = "AstrOsSerialMsgHandler";

AstrOsSerialMsgHandler AstrOs_SerialMsgHandler;

AstrOsSerialMsgHandler::AstrOsSerialMsgHandler() {}

AstrOsSerialMsgHandler::~AstrOsSerialMsgHandler() {}

void AstrOsSerialMsgHandler::Init(QueueHandle_t handlerQueue, QueueHandle_t serialQueue)
{
    this->handlerQueue = handlerQueue;
    this->serialQueue = serialQueue;

    this->msgService = AstrOsSerialMessageService();
}

void AstrOsSerialMsgHandler::handleMessage(std::string message)
{
    auto validation = this->msgService.validateSerialMsg(message);

    if (!validation.valid)
    {
        ESP_LOGE(TAG, "Invalid message: %s", message.c_str());
        return;
    }

    if (validation.type == AstrOsSerialMessageType::UNKNOWN)
    {
        ESP_LOGE(TAG, "Unknown/Invalid message type: %d", static_cast<int>(validation.type));
        return;
    }

    ESP_LOGD(TAG, "Received serial message type: %d", static_cast<int>(validation.type));

    // The pure decoder owns all of the field splitting and
    // per-controller validation. Everything ESP-specific (queue handoff,
    // logging) stays here at the boundary.
    auto decoded = AstrOsSerialProtocol::decodeSerialMessage(validation.type, validation.msgId, validation.payload);

    for (const auto &cmd : decoded.commands)
    {
        this->sendToInterfaceQueue(cmd.responseType, cmd.msgId, cmd.peerMac, cmd.peerName, cmd.message);
    }

    for (const auto &rej : decoded.rejects)
    {
        ESP_LOGW(TAG, "Invalid %s: %s", AstrOsSerialProtocol::describeRejectReason(rej.reason), rej.entry.c_str());
    }
}

/************************************
 * Send methods
 *************************************/

void AstrOsSerialMsgHandler::sendToInterfaceQueue(AstrOsInterfaceResponseType responseType, std::string msgId,
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

    if (xQueueSend(this->handlerQueue, &response, pdTICKS_TO_MS(250)) == pdFALSE)
    {
        ESP_LOGE(TAG, "Failed to send message to interface handler queue, type: %d", static_cast<int>(responseType));
        free(response.originationMsgId);
        free(response.peerMac);
        free(response.peerName);
        free(response.message);
    }
}

void AstrOsSerialMsgHandler::sendRegistraionAck(std::string msgId, std::vector<astros_peer_data_t> data)
{
    auto response = this->msgService.getRegistrationSyncAck(msgId, data);

    ESP_LOGD(TAG, "Sending registraion ack: %s", response.c_str());

    queue_serial_msg_t serialMsg;

    serialMsg.baudrate = 115200;
    serialMsg.message_id = 1;
    serialMsg.data = (uint8_t *)malloc(response.size() + 1);
    memcpy(serialMsg.data, response.c_str(), response.size());
    serialMsg.data[response.size()] = '\n';
    serialMsg.dataSize = response.size() + 1;

    if (xQueueSend(serialQueue, &serialMsg, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Send serial queue fail");
        free(serialMsg.data);
    }
}

void AstrOsSerialMsgHandler::sendPollAckNak(std::string mac, std::string name, std::string fingerprint, bool isAck)
{
    std::string response;

    if (isAck)
    {
        response = this->msgService.getPollAck(mac, name, fingerprint);
    }
    else
    {
        response = this->msgService.getPollNak(mac, name);
    }

    queue_serial_msg_t serialMsg;

    serialMsg.baudrate = 115200;
    serialMsg.message_id = 1;
    serialMsg.data = (uint8_t *)malloc(response.size() + 1);
    memcpy(serialMsg.data, response.c_str(), response.size());
    serialMsg.data[response.size()] = '\n';
    serialMsg.dataSize = response.size() + 1;

    if (xQueueSend(serialQueue, &serialMsg, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Send serial queue fail");
        free(serialMsg.data);
    }
}

void AstrOsSerialMsgHandler::sendBasicAckNakResponse(AstrOsSerialMessageType type, std::string msgId, std::string mac,
                                                     std::string name, std::string payload)
{
    auto response = this->msgService.getBasicAckNak(type, msgId, mac, name, payload);

    ESP_LOGD(TAG, "Sending response: %s", response.c_str());

    queue_serial_msg_t serialMsg;

    serialMsg.baudrate = 115200;
    serialMsg.message_id = 1;
    serialMsg.data = (uint8_t *)malloc(response.size() + 1);
    memcpy(serialMsg.data, response.c_str(), response.size());
    serialMsg.data[response.size()] = '\n';
    serialMsg.dataSize = response.size() + 1;

    if (xQueueSend(serialQueue, &serialMsg, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Send serial queue fail");
        free(serialMsg.data);
    }
}
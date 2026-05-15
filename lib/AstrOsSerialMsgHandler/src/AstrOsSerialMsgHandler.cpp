
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

void AstrOsSerialMsgHandler::Init(QueueHandle_t handlerQueue, QueueHandle_t serialQueue, QueueHandle_t otaQueue)
{
    this->handlerQueue = handlerQueue;
    this->serialQueue = serialQueue;
    this->otaQueue = otaQueue;

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
        ESP_LOGE(TAG, "Rejected controller record (%s) for message type %d: %s",
                 AstrOsSerialProtocol::describeRejectReason(rej.reason), static_cast<int>(validation.type),
                 rej.entry.c_str());
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

void AstrOsSerialMsgHandler::sendPollAckNak(std::string mac, std::string name, std::string fingerprint,
                                            std::string firmwareVersion, bool isAck)
{
    std::string response;

    if (isAck)
    {
        response = this->msgService.getPollAck(mac, name, fingerprint, firmwareVersion);
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

void AstrOsSerialMsgHandler::sendFwTransferBeginAck(std::string msgId, std::string transferId, std::string status)
{
    auto response = this->msgService.getFwTransferBeginAck(msgId, transferId, status);

    if (response.empty())
    {
        ESP_LOGE(TAG,
                 "FW_TRANSFER_BEGIN_ACK build returned empty — caller-contract violation. "
                 "msgId=%s transferId=%s status=%s",
                 msgId.c_str(), transferId.c_str(), status.c_str());
        return;
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
        ESP_LOGW(TAG, "Send serial queue fail (FW_TRANSFER_BEGIN_ACK)");
        free(serialMsg.data);
    }
}

void AstrOsSerialMsgHandler::sendFwChunkAck(std::string transferId, uint32_t highestContiguousSeq,
                                            uint32_t nextExpectedSeq, uint8_t windowRemaining)
{
    auto response = this->msgService.getFwChunkAck(transferId, highestContiguousSeq, nextExpectedSeq, windowRemaining);

    if (response.empty())
    {
        ESP_LOGE(TAG, "FW_CHUNK_ACK build returned empty — transferId=%s seq=%u", transferId.c_str(),
                 (unsigned)highestContiguousSeq);
        return;
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
        ESP_LOGW(TAG, "Send serial queue fail (FW_CHUNK_ACK)");
        free(serialMsg.data);
    }
}

void AstrOsSerialMsgHandler::sendFwChunkNak(std::string transferId, uint32_t lastGoodSeq, uint32_t nextExpectedSeq,
                                            std::string reasonCode)
{
    auto response = this->msgService.getFwChunkNak(transferId, lastGoodSeq, nextExpectedSeq, reasonCode);

    if (response.empty())
    {
        // getFwChunkNak rejects reasonCode not in {CRC, SIZE, OUT_OF_ORDER, FLASH_FULL}. This is a
        // fatal local bug — we can't legitimately recover, but we MUST emit *something* so the
        // server doesn't hang waiting on this seq. Fall back to "CRC" which is the safe
        // "ask for retransmit" default per the design doc's load-bearing contract.
        ESP_LOGE(TAG, "FW_CHUNK_NAK build returned empty for reasonCode='%s'. Falling back to CRC.",
                 reasonCode.c_str());
        response = this->msgService.getFwChunkNak(transferId, lastGoodSeq, nextExpectedSeq, "CRC");
        if (response.empty())
        {
            ESP_LOGE(TAG, "Fallback FW_CHUNK_NAK also empty — server will time out. Dropping reply.");
            return;
        }
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
        ESP_LOGW(TAG, "Send serial queue fail (FW_CHUNK_NAK)");
        free(serialMsg.data);
    }
}

void AstrOsSerialMsgHandler::sendFwTransferEndAck(std::string msgId, std::string transferId, std::string status,
                                                  std::string computedSha256Hex)
{
    auto response = this->msgService.getFwTransferEndAck(msgId, transferId, status, computedSha256Hex);

    if (response.empty())
    {
        ESP_LOGE(TAG, "FW_TRANSFER_END_ACK build returned empty — transferId=%s status=%s", transferId.c_str(),
                 status.c_str());
        return;
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
        ESP_LOGW(TAG, "Send serial queue fail (FW_TRANSFER_END_ACK)");
        free(serialMsg.data);
    }
}

void AstrOsSerialMsgHandler::sendFwDeployDone(std::string msgId, std::string transferId,
                                              std::vector<astros_fw_deploy_result_t> results)
{
    auto response = this->msgService.getFwDeployDone(msgId, transferId, results);

    if (response.empty())
    {
        // getFwDeployDone returns "" if results is empty or any status is not "OK"/"FAILED".
        // Both are caller programming errors. We can't legitimately recover — the server has no
        // sentinel to retry against, only timeout. Log and drop.
        ESP_LOGE(TAG, "FW_DEPLOY_DONE build returned empty — transferId=%s resultCount=%zu", transferId.c_str(),
                 results.size());
        return;
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
        ESP_LOGW(TAG, "Send serial queue fail (FW_DEPLOY_DONE)");
        free(serialMsg.data);
    }
}

#include <AstrOsInterfaceResponseMsg.hpp>
#include <AstrOsMessaging.hpp>
#include <AstrOsSerialMsgHandler.hpp>
#include <AstrOsSerialProtocol.hpp>
#include <AstrOsUtility.h>
#include <OtaQueueMessage.h>
#include <errno.h>
#include <esp_log.h>
#include <mbedtls/base64.h>
#include <stdlib.h>
#include <string.h>

const char *TAG = "AstrOsSerialMsgHandler";

AstrOsSerialMsgHandler AstrOs_SerialMsgHandler;

namespace
{
    // Returns a malloc'd, NUL-terminated copy of `s`. Caller frees. Returns
    // nullptr on malloc failure (caller must handle).
    char *dupString(const std::string &s)
    {
        char *p = (char *)malloc(s.size() + 1);
        if (p == nullptr)
        {
            return nullptr;
        }
        memcpy(p, s.c_str(), s.size());
        p[s.size()] = '\0';
        return p;
    }
} // namespace

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

    // FW_* OTA messages route through OtaReceiver's own queue, not the standard
    // decodeSerialMessage -> interfaceResponseQueue pipeline.
    switch (validation.type)
    {
    case AstrOsSerialMessageType::FW_TRANSFER_BEGIN:
        this->handleFwTransferBeginInbound(validation.msgId, validation.payload);
        return;
    case AstrOsSerialMessageType::FW_CHUNK:
        this->handleFwChunkInbound(validation.payload);
        return;
    case AstrOsSerialMessageType::FW_TRANSFER_END:
        this->handleFwTransferEndInbound(validation.msgId, validation.payload);
        return;
    case AstrOsSerialMessageType::FW_DEPLOY_BEGIN:
        this->handleFwDeployBeginInbound(validation.msgId, validation.payload);
        return;
    default:
        break;
    }

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
                                            std::string firmwareVersion, std::string variant, bool isAck)
{
    std::string response;

    if (isAck)
    {
        response = this->msgService.getPollAck(mac, name, fingerprint, firmwareVersion, variant);
    }
    else
    {
        // NAK path drops the variant (and fingerprint/version) — the server's
        // poll-nak parser is name-only, see message_handler.ts. Variant is
        // meaningless for a non-responding peer.
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
        // Builder rejects reasonCode outside {CRC, SIZE, OUT_OF_ORDER, FLASH_FULL}. Fall back
        // to "CRC" (safe retransmit) so the server doesn't hang waiting on this seq.
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

/************************************
 * FW_* inbound dispatch helpers
 *************************************/

void AstrOsSerialMsgHandler::handleFwTransferBeginInbound(const std::string &msgId, const std::string &payload)
{
    auto rec = parseFwTransferBegin(payload);
    if (!rec.valid)
    {
        ESP_LOGW(TAG, "FW_TRANSFER_BEGIN parse rejected payload");
        this->sendFwTransferBeginAck(msgId, /*transferId=*/"", "io_error");
        return;
    }

    // parseFwTransferBegin accepts chunkSize=0; dividing by it below would trap on ESP32.
    if (rec.chunkSize == 0)
    {
        ESP_LOGW(TAG, "FW_TRANSFER_BEGIN chunkSize=0 rejected (transferId=%s)", rec.transferId.c_str());
        this->sendFwTransferBeginAck(msgId, rec.transferId, "io_error");
        return;
    }

    queue_ota_msg_t m;
    memset(&m, 0, sizeof(m));
    m.kind = OTA_MSG_BEGIN;
    m.transferId = dupString(rec.transferId);
    m.begin.msgId = dupString(msgId);
    m.begin.totalSize = rec.totalSize;
    m.begin.totalChunks = AstrOsSerialProtocol::chunksForSize(rec.totalSize, rec.chunkSize);
    m.begin.chunkSize = rec.chunkSize;
    strncpy(m.begin.sha256Hex, rec.sha256Hex.c_str(), 64);
    m.begin.sha256Hex[64] = '\0';

    // Join targetIds back into a single RS-separated string for the consumer.
    std::string joined;
    for (size_t i = 0; i < rec.targetIds.size(); i++)
    {
        if (i > 0)
        {
            joined += '\x1E'; // RECORD_SEPARATOR
        }
        joined += rec.targetIds[i];
    }
    m.begin.targetList = dupString(joined);

    if (m.transferId == nullptr || m.begin.msgId == nullptr || m.begin.targetList == nullptr)
    {
        ESP_LOGE(TAG, "Malloc failed in FW_TRANSFER_BEGIN dispatch");
        free(m.transferId);
        free(m.begin.msgId);
        free(m.begin.targetList);
        this->sendFwTransferBeginAck(msgId, rec.transferId, "io_error");
        return;
    }

    if (xQueueSend(this->otaQueue, &m, pdMS_TO_TICKS(250)) != pdTRUE)
    {
        ESP_LOGW(TAG, "otaQueue full at FW_TRANSFER_BEGIN; rejecting with busy");
        free(m.transferId);
        free(m.begin.msgId);
        free(m.begin.targetList);
        this->sendFwTransferBeginAck(msgId, rec.transferId, "busy");
    }
}

void AstrOsSerialMsgHandler::handleFwChunkInbound(const std::string &payload)
{
    auto rec = parseFwChunk(payload);
    if (!rec.valid)
    {
        ESP_LOGW(TAG, "FW_CHUNK parse rejected payload");
        // No way to recover a transferId from a malformed payload — log and drop.
        return;
    }

    // mbedtls_base64_decode requires dst sized for the decoded output. The
    // declared wire-level payloadLen is the decoded byte count; allocate it
    // exactly and treat any size mismatch from the decoder as SIZE failure.
    uint8_t *decoded = (uint8_t *)malloc(rec.payloadLen);
    if (decoded == nullptr)
    {
        ESP_LOGE(TAG, "Malloc failed in FW_CHUNK dispatch");
        this->sendFwChunkNak(rec.transferId, /*lastGoodSeq=*/0, /*nextExpectedSeq=*/rec.seq, "SIZE");
        return;
    }

    size_t outLen = 0;
    int rc = mbedtls_base64_decode(decoded, rec.payloadLen, &outLen,
                                   reinterpret_cast<const unsigned char *>(rec.base64Payload.data()),
                                   rec.base64Payload.size());
    if (rc != 0 || outLen != rec.payloadLen)
    {
        // Each branch below is a protocol contract violation by the server — wire-declared
        // payloadLen disagrees with the actual decoded length, or the base64 string contains
        // a character the protocol forbids. LOGE so a single occurrence is visible; the
        // SIZE NAK is the same as before.
        if (rc == MBEDTLS_ERR_BASE64_INVALID_CHARACTER)
        {
            ESP_LOGE(TAG, "FW_CHUNK base64 invalid character (seq=%u transferId=%s base64Len=%zu)", (unsigned)rec.seq,
                     rec.transferId.c_str(), rec.base64Payload.size());
        }
        else if (rc == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL)
        {
            ESP_LOGE(TAG, "FW_CHUNK declared payloadLen=%u too small for base64 input=%zu (seq=%u transferId=%s)",
                     (unsigned)rec.payloadLen, rec.base64Payload.size(), (unsigned)rec.seq, rec.transferId.c_str());
        }
        else
        {
            ESP_LOGE(TAG, "FW_CHUNK base64 size mismatch out=%zu expected=%u rc=%d (seq=%u transferId=%s)", outLen,
                     (unsigned)rec.payloadLen, rc, (unsigned)rec.seq, rec.transferId.c_str());
        }
        free(decoded);
        this->sendFwChunkNak(rec.transferId, /*lastGoodSeq=*/0, /*nextExpectedSeq=*/rec.seq, "SIZE");
        return;
    }

    queue_ota_msg_t m;
    memset(&m, 0, sizeof(m));
    m.kind = OTA_MSG_CHUNK;
    m.transferId = dupString(rec.transferId);
    m.chunk.seq = rec.seq;
    m.chunk.payloadLen = rec.payloadLen;
    m.chunk.crc16 = rec.crc16;
    m.chunk.payload = decoded;

    if (m.transferId == nullptr)
    {
        ESP_LOGE(TAG, "Malloc failed in FW_CHUNK dispatch (transferId)");
        free(decoded);
        this->sendFwChunkNak(rec.transferId, /*lastGoodSeq=*/0, /*nextExpectedSeq=*/rec.seq, "SIZE");
        return;
    }

    if (xQueueSend(this->otaQueue, &m, pdMS_TO_TICKS(50)) != pdTRUE)
    {
        // Queue full — emit CRC NAK so the server retransmits once the receiver drains.
        ESP_LOGW(TAG, "otaQueue full at FW_CHUNK seq=%u; emitting CRC NAK to force retransmit", (unsigned)rec.seq);
        free(m.transferId);
        free(decoded);
        this->sendFwChunkNak(rec.transferId, /*lastGoodSeq=*/0, /*nextExpectedSeq=*/rec.seq, "CRC");
    }
}

void AstrOsSerialMsgHandler::handleFwTransferEndInbound(const std::string &msgId, const std::string &payload)
{
    auto rec = parseFwTransferEnd(payload);
    if (!rec.valid)
    {
        ESP_LOGW(TAG, "FW_TRANSFER_END parse rejected payload");
        this->sendFwTransferEndAck(msgId, /*transferId=*/"", "IO_ERROR",
                                   "0000000000000000000000000000000000000000000000000000000000000000");
        return;
    }

    queue_ota_msg_t m;
    memset(&m, 0, sizeof(m));
    m.kind = OTA_MSG_END;
    m.transferId = dupString(rec.transferId);
    m.end.msgId = dupString(msgId);
    m.end.totalChunks = rec.totalChunks;
    strncpy(m.end.finalSha256Hex, rec.finalSha256Hex.c_str(), 64);
    m.end.finalSha256Hex[64] = '\0';

    if (m.transferId == nullptr || m.end.msgId == nullptr)
    {
        ESP_LOGE(TAG, "Malloc failed in FW_TRANSFER_END dispatch");
        free(m.transferId);
        free(m.end.msgId);
        this->sendFwTransferEndAck(msgId, rec.transferId, "IO_ERROR", rec.finalSha256Hex);
        return;
    }

    // 100ms — matches CHUNK's tight bound. END arrives right after the chunk
    // stream completes, so a wedged consumer at this point means astrosRxTask
    // would otherwise stall for 500ms while inbound bytes pile into the UART RX
    // buffer (8 KB; see src/main.cpp). Server retries on IO_ERROR.
    if (xQueueSend(this->otaQueue, &m, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        ESP_LOGE(TAG, "otaQueue full at FW_TRANSFER_END");
        free(m.transferId);
        free(m.end.msgId);
        this->sendFwTransferEndAck(msgId, rec.transferId, "IO_ERROR", rec.finalSha256Hex);
    }
}

void AstrOsSerialMsgHandler::handleFwDeployBeginInbound(const std::string &msgId, const std::string &payload)
{
    auto rec = parseFwDeployBegin(payload);
    if (!rec.valid)
    {
        ESP_LOGW(TAG, "FW_DEPLOY_BEGIN parse rejected payload");
        std::vector<astros_fw_deploy_result_t> empty; // sendFwDeployDone drops on empty, intentional
        this->sendFwDeployDone(msgId, /*transferId=*/"", empty);
        return;
    }

    std::string joined;
    for (size_t i = 0; i < rec.orderIds.size(); i++)
    {
        if (i > 0)
        {
            joined += '\x1E';
        }
        joined += rec.orderIds[i];
    }

    queue_ota_msg_t m;
    memset(&m, 0, sizeof(m));
    m.kind = OTA_MSG_DEPLOY_BEGIN;
    m.transferId = dupString(rec.transferId);
    m.deploy.msgId = dupString(msgId);
    m.deploy.orderList = dupString(joined);

    if (m.transferId == nullptr || m.deploy.msgId == nullptr || m.deploy.orderList == nullptr)
    {
        ESP_LOGE(TAG, "Malloc failed in FW_DEPLOY_BEGIN dispatch");
        free(m.transferId);
        free(m.deploy.msgId);
        free(m.deploy.orderList);
        // Synthesize a FAILED result per target so JobLock can release.
        std::vector<astros_fw_deploy_result_t> failures;
        for (const auto &id : rec.orderIds)
        {
            failures.push_back({id, "FAILED", "", "io_error"});
        }
        this->sendFwDeployDone(msgId, rec.transferId, failures);
        return;
    }

    if (xQueueSend(this->otaQueue, &m, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        ESP_LOGW(TAG, "otaQueue full at FW_DEPLOY_BEGIN");
        free(m.transferId);
        free(m.deploy.msgId);
        free(m.deploy.orderList);
        std::vector<astros_fw_deploy_result_t> failures;
        for (const auto &id : rec.orderIds)
        {
            failures.push_back({id, "FAILED", "", "io_error"});
        }
        this->sendFwDeployDone(msgId, rec.transferId, failures);
    }
}
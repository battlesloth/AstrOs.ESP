#include <OtaReceiver.hpp>

#include <AstrOsSerialMsgHandler.hpp>
#include <AstrOsStringUtils.hpp>
#include <esp_log.h>

static const char *TAG = "OtaReceiver";

OtaReceiver AstrOs_OtaReceiver;

OtaReceiver::OtaReceiver() {}

OtaReceiver::~OtaReceiver() {}

void OtaReceiver::Init()
{
    // Phase 3: nothing to do here yet. Phase 4 will allocate the SHA
    // context and ensure the firmware staging dir exists on SD.
    ESP_LOGI(TAG, "OtaReceiver initialized");
}

void OtaReceiver::process(queue_ota_msg_t &msg)
{
    switch (msg.kind)
    {
    case OTA_MSG_BEGIN:
        handleBegin(msg);
        break;
    case OTA_MSG_CHUNK:
        handleChunk(msg);
        break;
    case OTA_MSG_END:
        handleEnd(msg);
        break;
    case OTA_MSG_DEPLOY_BEGIN:
        handleDeployBegin(msg);
        break;
    default:
        // Unknown kind means either memory corruption on the queue or a new
        // OTA_MSG_* enumerator was added without extending this switch. Both
        // are programmer errors: silently dropping the message would leak the
        // union arm's pointers AND leave the server waiting for a reply. Fail
        // loud — abort surfaces the panic trace through the existing
        // esp32_exception_decoder pipeline.
        ESP_LOGE(TAG, "Unknown ota_msg_kind_t: %d transferId=%s — protocol violation, aborting",
                 static_cast<int>(msg.kind), msg.transferId ? msg.transferId : "(null)");
        free(msg.transferId);
        abort();
    }
}

void OtaReceiver::handleBegin(queue_ota_msg_t &msg)
{
    std::string msgId = msg.begin.msgId ? msg.begin.msgId : "";
    std::string transferIdIn = msg.transferId ? msg.transferId : "";

    if (active_)
    {
        ESP_LOGW(TAG, "FW_TRANSFER_BEGIN while transfer %s active; replying busy", transferIdStr_.c_str());
        AstrOs_SerialMsgHandler.sendFwTransferBeginAck(msgId, transferIdIn, "busy");
        free(msg.begin.msgId);
        free(msg.begin.targetList);
        free(msg.transferId);
        return;
    }

    // Convert the wire-level opaque string transferId to BulkReceiver's uint8_t form.
    auto parsed = AstrOsStringUtils::parseStrictU8(transferIdIn);
    if (!parsed.has_value())
    {
        ESP_LOGE(TAG, "FW_TRANSFER_BEGIN transferId='%s' is not 0..255 numeric", transferIdIn.c_str());
        AstrOs_SerialMsgHandler.sendFwTransferBeginAck(msgId, transferIdIn, "io_error");
        free(msg.begin.msgId);
        free(msg.begin.targetList);
        free(msg.transferId);
        return;
    }
    uint8_t xferId = parsed.value();

    // Phase 3 sliding window: hard-coded to 16 (matches the server's nominal sender window).
    // Phase 5 may make this configurable.
    constexpr uint8_t kPhase3WindowSize = 16;

    auto br = bulk_.begin(xferId, msg.begin.totalSize, msg.begin.totalChunks, msg.begin.chunkSize, kPhase3WindowSize);
    if (!br.valid)
    {
        ESP_LOGW(TAG, "BulkReceiver::begin rejected: reason=%d (totalSize=%u chunks=%u chunkSize=%u)",
                 static_cast<int>(br.reason), (unsigned)msg.begin.totalSize, (unsigned)msg.begin.totalChunks,
                 (unsigned)msg.begin.chunkSize);
        AstrOs_SerialMsgHandler.sendFwTransferBeginAck(msgId, transferIdIn, "io_error");
        free(msg.begin.msgId);
        free(msg.begin.targetList);
        free(msg.transferId);
        return;
    }

    active_ = true;
    transferIdStr_ = transferIdIn;
    beginMsgId_ = msgId;

    ESP_LOGI(TAG, "FW_TRANSFER_BEGIN accepted: transferId=%s totalSize=%u chunks=%u sha=%s", transferIdIn.c_str(),
             (unsigned)msg.begin.totalSize, (unsigned)msg.begin.totalChunks, msg.begin.sha256Hex);

    AstrOs_SerialMsgHandler.sendFwTransferBeginAck(msgId, transferIdIn, "OK");

    free(msg.begin.msgId);
    free(msg.begin.targetList);
    free(msg.transferId);
}

void OtaReceiver::handleChunk(queue_ota_msg_t &msg)
{
    std::string transferIdIn = msg.transferId ? msg.transferId : "";

    if (!active_)
    {
        ESP_LOGW(TAG, "FW_CHUNK while no transfer active; emitting inactive NAK");
        AstrOs_SerialMsgHandler.sendFwChunkNak(transferIdIn, /*lastGoodSeq=*/0, /*nextExpectedSeq=*/0, "OUT_OF_ORDER");
        free(msg.chunk.payload);
        free(msg.transferId);
        return;
    }

    // Re-derive the uint8_t form. transferIdStr_ is the authoritative running
    // value — if the incoming string mismatches, BulkReceiver::onChunk's
    // internal xferId check will return nakInactive() with reason=OUT_OF_ORDER.
    auto parsedChunk = AstrOsStringUtils::parseStrictU8(transferIdIn);
    if (!parsedChunk.has_value())
    {
        // Wire form was numeric at BEGIN time; arriving as garbage now is a server bug.
        ESP_LOGW(TAG, "FW_CHUNK transferId='%s' not numeric; emitting OUT_OF_ORDER", transferIdIn.c_str());
        AstrOs_SerialMsgHandler.sendFwChunkNak(transferIdIn, /*lastGoodSeq=*/0, /*nextExpectedSeq=*/0, "OUT_OF_ORDER");
        free(msg.chunk.payload);
        free(msg.transferId);
        return;
    }
    uint8_t xferId = parsedChunk.value();

    auto cr = bulk_.onChunk(xferId, msg.chunk.seq, msg.chunk.payloadLen, msg.chunk.crc16, msg.chunk.payload);

    if (cr.decision == AstrOsBulkTransport::Decision::ACK)
    {
        // Phase 3 sinks payload to /dev/null. Phase 4 will, in this branch,
        // before the sendFwChunkAck call:
        //   - fwrite(cr.payload, cr.payloadLen, 1, stagingFp_)
        //   - mbedtls_sha256_update(&shaCtx_, cr.payload, cr.payloadLen)
        // and on fwrite failure must NAK with reason=FLASH_FULL instead of
        // ACK — DO NOT drop a bare fwrite in here without the failure branch.
        AstrOs_SerialMsgHandler.sendFwChunkAck(transferIdIn, cr.highestContiguousSeq, cr.nextExpectedSeq,
                                               cr.windowRemaining);
    }
    else
    {
        const char *reasonStr = "OUT_OF_ORDER";
        switch (cr.reason)
        {
        case AstrOsBulkTransport::NakReason::CRC:
            reasonStr = "CRC";
            break;
        case AstrOsBulkTransport::NakReason::SIZE:
            reasonStr = "SIZE";
            break;
        case AstrOsBulkTransport::NakReason::OUT_OF_ORDER:
            reasonStr = "OUT_OF_ORDER";
            break;
        case AstrOsBulkTransport::NakReason::FLASH_FULL:
            reasonStr = "FLASH_FULL";
            break;
        case AstrOsBulkTransport::NakReason::NONE:
            // Unreachable on the NAK path — log and force CRC for safe retransmit.
            ESP_LOGE(TAG, "BulkReceiver returned NAK with reason=NONE; forcing CRC");
            reasonStr = "CRC";
            break;
        default:
            // Forward-compat: if Phase 4+ adds a new NakReason enumerator and forgets to
            // extend this switch, surface it loud instead of silently downgrading to CRC.
            ESP_LOGE(TAG, "Unknown NakReason value %d — forcing CRC for safe retransmit", static_cast<int>(cr.reason));
            reasonStr = "CRC";
            break;
        }
        AstrOs_SerialMsgHandler.sendFwChunkNak(transferIdIn, cr.highestContiguousSeq, cr.nextExpectedSeq, reasonStr);
    }

    free(msg.chunk.payload);
    free(msg.transferId);
}

void OtaReceiver::handleEnd(queue_ota_msg_t &msg)
{
    std::string transferIdIn = msg.transferId ? msg.transferId : "";
    std::string msgId = msg.end.msgId ? msg.end.msgId : "";
    std::string finalShaIn(msg.end.finalSha256Hex);

    auto parsedEnd = AstrOsStringUtils::parseStrictU8(transferIdIn);
    if (!parsedEnd.has_value())
    {
        ESP_LOGW(TAG, "FW_TRANSFER_END transferId='%s' not numeric", transferIdIn.c_str());
        AstrOs_SerialMsgHandler.sendFwTransferEndAck(msgId, transferIdIn, "IO_ERROR", finalShaIn);
        active_ = false;
        transferIdStr_.clear();
        beginMsgId_.clear();
        bulk_.reset();
        free(msg.end.msgId);
        free(msg.transferId);
        return;
    }
    uint8_t xferId = parsedEnd.value();

    auto er = bulk_.onEnd(xferId, msg.end.totalChunks);

    if (er.status == AstrOsBulkTransport::EndResult::Status::OK)
    {
        // Phase 3: no streaming SHA, so we echo the server's stated hash back
        // as the "computed" value. Phase 4 will replace this with the real
        // mbedtls_sha256_finish output.
        ESP_LOGI(TAG, "FW_TRANSFER_END OK: transferId=%s totalChunks=%u", transferIdIn.c_str(),
                 (unsigned)msg.end.totalChunks);
        AstrOs_SerialMsgHandler.sendFwTransferEndAck(msgId, transferIdIn, "OK", finalShaIn);
    }
    else
    {
        // Decode reason to a string and pick severity: WRONG_XFER_ID and
        // SENDER_TOTAL_MISMATCH point at a server-state bug (the server confused
        // its own books) and deserve LOGE so they surface in monitoring.
        // RECEIVER_SHORT_COUNT and NOT_ACTIVE are transient-or-local conditions
        // and stay at LOGW.
        const char *reasonStr = "(unknown)";
        bool serverBug = false;
        switch (er.reason)
        {
        case AstrOsBulkTransport::EndResult::Reason::NONE:
            reasonStr = "NONE";
            break;
        case AstrOsBulkTransport::EndResult::Reason::NOT_ACTIVE:
            reasonStr = "NOT_ACTIVE";
            break;
        case AstrOsBulkTransport::EndResult::Reason::WRONG_XFER_ID:
            reasonStr = "WRONG_XFER_ID";
            serverBug = true;
            break;
        case AstrOsBulkTransport::EndResult::Reason::SENDER_TOTAL_MISMATCH:
            reasonStr = "SENDER_TOTAL_MISMATCH";
            serverBug = true;
            break;
        case AstrOsBulkTransport::EndResult::Reason::RECEIVER_SHORT_COUNT:
            reasonStr = "RECEIVER_SHORT_COUNT";
            break;
        default:
            // Forward-compat: a new Reason enumerator added without extending this switch
            // would silently log at LOGW with reasonStr="(unknown)". Surface it loud so the
            // miss is caught in monitoring instead of slipping past as a benign warning.
            ESP_LOGE(TAG, "Unknown EndResult::Reason value %d — please extend the switch", static_cast<int>(er.reason));
            serverBug = true;
            break;
        }
        if (serverBug)
        {
            ESP_LOGE(TAG, "FW_TRANSFER_END IO_ERROR reason=%s (transferId=%s server-state bug)", reasonStr,
                     transferIdIn.c_str());
        }
        else
        {
            ESP_LOGW(TAG, "FW_TRANSFER_END IO_ERROR reason=%s (transferId=%s)", reasonStr, transferIdIn.c_str());
        }
        AstrOs_SerialMsgHandler.sendFwTransferEndAck(msgId, transferIdIn, "IO_ERROR", finalShaIn);
    }

    active_ = false;
    transferIdStr_.clear();
    beginMsgId_.clear();
    bulk_.reset();

    free(msg.end.msgId);
    free(msg.transferId);
}

void OtaReceiver::handleDeployBegin(queue_ota_msg_t &msg)
{
    std::string transferIdIn = msg.transferId ? msg.transferId : "";
    std::string msgId = msg.deploy.msgId ? msg.deploy.msgId : "";
    std::string orderListStr = msg.deploy.orderList ? msg.deploy.orderList : "";

    std::vector<astros_fw_deploy_result_t> results;
    if (orderListStr.empty())
    {
        // Empty order list = server error. Don't send anything (sendFwDeployDone drops on empty).
        ESP_LOGE(TAG, "FW_DEPLOY_BEGIN orderList empty — dropping");
    }
    else
    {
        size_t start = 0;
        while (start < orderListStr.size())
        {
            size_t end = orderListStr.find('\x1E', start);
            std::string id =
                (end == std::string::npos) ? orderListStr.substr(start) : orderListStr.substr(start, end - start);
            if (!id.empty())
            {
                results.push_back({id, "FAILED", "", "not_implemented"});
            }
            if (end == std::string::npos)
            {
                break;
            }
            start = end + 1;
        }
    }

    ESP_LOGI(TAG, "FW_DEPLOY_BEGIN stub: transferId=%s target-count=%zu — all-FAILED not_implemented",
             transferIdIn.c_str(), results.size());

    AstrOs_SerialMsgHandler.sendFwDeployDone(msgId, transferIdIn, results);

    free(msg.deploy.msgId);
    free(msg.deploy.orderList);
    free(msg.transferId);
}

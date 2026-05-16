#include <OtaReceiver.hpp>

#include <AstrOsSerialMsgHandler.hpp>
#include <AstrOsStringUtils.hpp>
#include <esp_log.h>

static const char *TAG = "OtaReceiver";

OtaReceiver AstrOs_OtaReceiver;

OtaReceiver::OtaReceiver() {}

OtaReceiver::~OtaReceiver() {}

void OtaReceiver::Init(QueueHandle_t otaQueue)
{
    otaQueue_ = otaQueue;

    const esp_timer_create_args_t args = {
        .callback = &OtaReceiver::watchdogTimerCb,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ota_receiver_watchdog",
        .skip_unhandled_events = false,
    };
    esp_err_t err = esp_timer_create(&args, &watchdog_);
    if (err != ESP_OK)
    {
        // Create-failure is a programmer error (timer count exhausted is the only
        // realistic cause). Log + leave watchdog_ null — receiver still works for
        // happy-path transfers; only the stuck-recovery path is lost.
        ESP_LOGE(TAG, "esp_timer_create(ota_receiver_watchdog) failed: %s — watchdog disabled", esp_err_to_name(err));
        watchdog_ = nullptr;
    }

    ESP_LOGI(TAG, "OtaReceiver initialized (watchdog idle threshold: %llums)", kWatchdogIdleUs / 1000ULL);
}

void OtaReceiver::watchdogTimerCb(void *arg)
{
    auto self = static_cast<OtaReceiver *>(arg);
    if (self->otaQueue_ == nullptr)
    {
        // Init() failed to capture the queue or wasn't called. Nothing safe
        // to do from the timer dispatch task; log and bail.
        return;
    }
    queue_ota_msg_t msg{};
    msg.kind = OTA_MSG_WATCHDOG_FIRE;
    msg.transferId = nullptr;
    // Non-blocking send from the timer dispatch task — if the queue is full,
    // otaReceiverTask is already wedged and dropping this signal is the
    // least-bad outcome (the wedge surfaces through the next operator action).
    if (xQueueSend(self->otaQueue_, &msg, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "watchdog: otaQueue full, dropping WATCHDOG_FIRE signal");
    }
}

void OtaReceiver::watchdogStart()
{
    if (watchdog_ == nullptr)
        return;
    esp_err_t err = esp_timer_start_once(watchdog_, kWatchdogIdleUs);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "watchdog: esp_timer_start_once failed: %s", esp_err_to_name(err));
    }
}

void OtaReceiver::watchdogRestart()
{
    if (watchdog_ == nullptr)
        return;
    // esp_timer doesn't expose a single "restart one-shot" call. Stop + start
    // is the documented pattern. esp_timer_stop on an idle timer is a no-op
    // returning ESP_ERR_INVALID_STATE — safe to ignore.
    esp_timer_stop(watchdog_);
    esp_err_t err = esp_timer_start_once(watchdog_, kWatchdogIdleUs);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "watchdog: esp_timer_start_once (restart) failed: %s — transfer no longer protected",
                 esp_err_to_name(err));
    }
}

void OtaReceiver::watchdogStop()
{
    if (watchdog_ == nullptr)
        return;
    esp_timer_stop(watchdog_);
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
    case OTA_MSG_WATCHDOG_FIRE:
        // transferId is nullptr by contract; defensive free() in case a future producer
        // mistakenly sets it. free(nullptr) is a no-op.
        free(msg.transferId);
        handleWatchdogFire();
        break;
    default:
        // Unknown kind: a forgotten case label, not memory corruption. LOGE + return so
        // a missed enumerator surfaces in logs without bricking the box. The union arm's
        // ownership is unknown, so only the transferId can be safely freed.
        ESP_LOGE(TAG, "Unknown ota_msg_kind_t: %d transferId=%s — dropping", static_cast<int>(msg.kind),
                 msg.transferId ? msg.transferId : "(null)");
        free(msg.transferId);
        return;
    }
}

void OtaReceiver::handleWatchdogFire()
{
    if (!active_)
    {
        // Stale fire — handleEnd or a prior watchdog already cleared the
        // state but this signal was already in flight before we stopped the
        // timer. Idempotent no-op is the correct behavior.
        return;
    }
    ESP_LOGW(TAG, "OTA watchdog fired: transferId=%s idle >%llums — aborting transfer", transferIdStr_.c_str(),
             kWatchdogIdleUs / 1000ULL);
    bulk_.reset();
    active_ = false;
    transferIdStr_.clear();
    // Don't notify the server — its chunk_streamer has its own timeout that
    // surfaces the failure to the operator. The master's job is to be
    // reachable for the next BEGIN.
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

    // Wire-level transferId is an opaque string; BulkReceiver wants a uint8_t. parseStrictU8
    // tightens the contract (rejects whitespace + signed forms that strtoul would accept).
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

    // Matches the server's nominal sender window.
    constexpr uint8_t kWindowSize = 16;

    auto br = bulk_.begin(xferId, msg.begin.totalSize, msg.begin.totalChunks, msg.begin.chunkSize, kWindowSize);
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

    ESP_LOGI(TAG, "FW_TRANSFER_BEGIN accepted: transferId=%s totalSize=%u chunks=%u sha=%s", transferIdIn.c_str(),
             (unsigned)msg.begin.totalSize, (unsigned)msg.begin.totalChunks, msg.begin.sha256Hex);

    AstrOs_SerialMsgHandler.sendFwTransferBeginAck(msgId, transferIdIn, "OK");

    // Start the idle-activity watchdog. If no FW_CHUNK arrives within
    // kWatchdogIdleUs, watchdogTimerCb posts OTA_MSG_WATCHDOG_FIRE and the
    // receiver clears state so the next BEGIN can be accepted.
    watchdogStart();

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

    // If the incoming string mismatches the running transfer's xferId, BulkReceiver::onChunk
    // will return nakInactive() with reason=OUT_OF_ORDER.
    auto parsedChunk = AstrOsStringUtils::parseStrictU8(transferIdIn);
    if (!parsedChunk.has_value())
    {
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
            // Unknown enumerator: fall back to CRC for safe retransmit; loud log so the miss
            // is caught before it ships.
            ESP_LOGE(TAG, "Unknown NakReason value %d — forcing CRC", static_cast<int>(cr.reason));
            reasonStr = "CRC";
            break;
        }
        AstrOs_SerialMsgHandler.sendFwChunkNak(transferIdIn, cr.highestContiguousSeq, cr.nextExpectedSeq, reasonStr);
    }

    // Any chunk activity (ACK or NAK) counts as "transfer is alive" — reset
    // the watchdog. A long string of NAKs alone wouldn't drain the transfer,
    // but the server's chunk_streamer has its own retry-exhaustion failure
    // for that. The watchdog protects against *silence*, not slow progress.
    watchdogRestart();

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
        // Unparseable transferId — we can't tell whether this END is for our
        // in-flight transfer or a stray. NAK without touching state so a real
        // transfer in progress isn't torn down by a malformed END.
        ESP_LOGW(TAG, "FW_TRANSFER_END transferId='%s' not numeric; emitting IO_ERROR (state preserved)",
                 transferIdIn.c_str());
        AstrOs_SerialMsgHandler.sendFwTransferEndAck(msgId, transferIdIn, "IO_ERROR", finalShaIn);
        free(msg.end.msgId);
        free(msg.transferId);
        return;
    }
    uint8_t xferId = parsedEnd.value();

    auto er = bulk_.onEnd(xferId, msg.end.totalChunks);

    if (er.status == AstrOsBulkTransport::EndResult::Status::OK)
    {
        // No local hash is computed yet — echo the server's stated hash back as the "computed"
        // value so the protocol-level comparison succeeds.
        ESP_LOGI(TAG, "FW_TRANSFER_END OK: transferId=%s totalChunks=%u", transferIdIn.c_str(),
                 (unsigned)msg.end.totalChunks);
        AstrOs_SerialMsgHandler.sendFwTransferEndAck(msgId, transferIdIn, "OK", finalShaIn);
    }
    else
    {
        // WRONG_XFER_ID and SENDER_TOTAL_MISMATCH indicate the server confused its own state —
        // surface those at LOGE. Other reasons stay at LOGW.
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
            // Unknown enumerator: promote to LOGE so the miss isn't masked as a benign warning.
            ESP_LOGE(TAG, "Unknown EndResult::Reason value %d", static_cast<int>(er.reason));
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

    // Only tear down in-flight state when bulk_.onEnd indicates the END applied
    // to our running transfer. WRONG_XFER_ID / NOT_ACTIVE mean the END was stray
    // (server bug, late retry, or no transfer in flight) and must NOT clobber a
    // healthy in-progress transfer.
    const bool endForOurTransfer = er.status == AstrOsBulkTransport::EndResult::Status::OK ||
                                   er.reason == AstrOsBulkTransport::EndResult::Reason::SENDER_TOTAL_MISMATCH ||
                                   er.reason == AstrOsBulkTransport::EndResult::Reason::RECEIVER_SHORT_COUNT;

    if (endForOurTransfer)
    {
        active_ = false;
        transferIdStr_.clear();
        bulk_.reset();

        // Transfer complete — stop the idle watchdog so a stale fire doesn't
        // arrive after we've already cleared state. handleWatchdogFire is
        // idempotent against !active_, but stopping is the explicit signal.
        watchdogStop();
    }

    free(msg.end.msgId);
    free(msg.transferId);
}

void OtaReceiver::handleDeployBegin(queue_ota_msg_t &msg)
{
    std::string transferIdIn = msg.transferId ? msg.transferId : "";
    std::string msgId = msg.deploy.msgId ? msg.deploy.msgId : "";
    std::string orderListStr = msg.deploy.orderList ? msg.deploy.orderList : "";

    if (orderListStr.empty())
    {
        // sendFwDeployDone drops on empty results — skip the call so we don't
        // double-log. Server times out and surfaces the failure to the operator.
        ESP_LOGE(TAG, "FW_DEPLOY_BEGIN orderList empty — dropping");
        free(msg.deploy.msgId);
        free(msg.deploy.orderList);
        free(msg.transferId);
        return;
    }

    std::vector<astros_fw_deploy_result_t> results;
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

    ESP_LOGI(TAG, "FW_DEPLOY_BEGIN: transferId=%s target-count=%zu — all-FAILED not_implemented", transferIdIn.c_str(),
             results.size());

    AstrOs_SerialMsgHandler.sendFwDeployDone(msgId, transferIdIn, results);

    free(msg.deploy.msgId);
    free(msg.deploy.orderList);
    free(msg.transferId);
}

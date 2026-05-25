#include "OtaForwarder.hpp"

#include <AstrOsEspNow.h>
#include <AstrOsMessaging.hpp>
#include <AstrOsSerialMsgHandler.hpp>
#include <AstrOsSha256.h>
#include <AstrOsStringUtils.hpp>
#include <OtaReceiver.hpp>
#include <esp_log.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <sys/stat.h>

namespace
{
    constexpr const char *TAG = "OtaForwarder";
} // namespace

OtaForwarder AstrOs_OtaForwarder;

OtaForwarder::OtaForwarder() = default;

OtaForwarder::~OtaForwarder()
{
    // Singleton is process-lifetime, so this typically doesn't run. Guards
    // against the timer-leak hazard if a test fixture ever instantiates a
    // non-singleton OtaForwarder; mirrors the April 2026 code review's
    // concern about latent leaks in similar singletons.
    for (esp_timer_handle_t *t : {&tickTimer_, &beginAckTimer_, &endAckTimer_})
    {
        if (*t)
        {
            esp_timer_stop(*t);
            esp_timer_delete(*t);
            *t = nullptr;
        }
    }
}

void OtaForwarder::Init(QueueHandle_t otaForwarderQueue)
{
    // Idempotent: Init is called exactly once during boot today, but a guard
    // prevents the timer-handle leak that would otherwise occur on a
    // hypothetical double-Init.
    if (tickTimer_ != nullptr)
    {
        return;
    }

    otaForwarderQueue_ = otaForwarderQueue;

    // Create timers eagerly; start/stop them per transfer.
    esp_timer_create_args_t tickArgs = {
        .callback = &OtaForwarder::tickTimerCb,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ota_fwd_tick",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&tickArgs, &tickTimer_));

    esp_timer_create_args_t beginArgs = {
        .callback = &OtaForwarder::beginAckTimerCb,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ota_fwd_begin_ack",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&beginArgs, &beginAckTimer_));

    esp_timer_create_args_t endArgs = {
        .callback = &OtaForwarder::endAckTimerCb,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ota_fwd_end_ack",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&endArgs, &endAckTimer_));
}

void OtaForwarder::process(queue_ota_forwarder_msg_t &msg)
{
    switch (msg.kind)
    {
    case OTA_FWD_DEPLOY_BEGIN:
        handleDeployBegin(msg);
        break;
    case OTA_FWD_BEGIN_ACK:
        handleBeginAck(msg);
        break;
    case OTA_FWD_BEGIN_NAK:
        handleBeginNak(msg);
        break;
    case OTA_FWD_DATA_ACK:
        handleDataAck(msg);
        break;
    case OTA_FWD_DATA_NAK:
        handleDataNak(msg);
        break;
    case OTA_FWD_END_ACK:
        handleEndAck(msg);
        break;
    case OTA_FWD_TICK:
        handleTick();
        break;
    default:
        ESP_LOGE(TAG, "process: unknown kind %d", (int)msg.kind);
        break;
    }
    freeOtaForwarderMsg(&msg);
}

void OtaForwarder::handleDeployBegin(queue_ota_forwarder_msg_t &msg)
{
    if (phase_ != Phase::IDLE)
    {
        ESP_LOGW(TAG, "handleDeployBegin while not IDLE (phase=%d); rejecting", (int)phase_);
        // JobLock expects one result per controller-id in the order list;
        // synthesizing a single "unknown" row would leave the server state
        // inconsistent for a multi-target deploy. Parse the order list so
        // each target gets a matching FAILED("forwarder_busy") row.
        auto rejectedIds = AstrOsStringUtils::parseOrderList(msg.deploy.orderList);
        std::vector<astros_fw_deploy_result_t> failures;
        failures.reserve(rejectedIds.empty() ? 1 : rejectedIds.size());
        if (rejectedIds.empty())
        {
            // No order list parsed (malformed or null) — fall back to a single
            // synthetic row so the server still sees something terminal.
            failures.push_back({"unknown", "FAILED", "", "forwarder_busy"});
        }
        else
        {
            for (const auto &id : rejectedIds)
            {
                failures.push_back({id, "FAILED", "", "forwarder_busy"});
            }
        }
        std::string msgId = msg.deploy.msgId ? msg.deploy.msgId : "";
        std::string xferId = msg.transferId ? msg.transferId : "";
        AstrOs_SerialMsgHandler.sendFwDeployDone(msgId, xferId, failures);
        return;
    }

    deployMsgId_ = msg.deploy.msgId ? msg.deploy.msgId : "";
    deployTransferId_ = msg.transferId ? msg.transferId : "";

    orderList_ = AstrOsStringUtils::parseOrderList(msg.deploy.orderList);

    if (orderList_.empty())
    {
        // Mirror the busy-reject fallback: an empty order list with no
        // targets would otherwise ship a 0-row FW_DEPLOY_DONE which the
        // server's JobLock could interpret as silent success. One
        // synthetic FAILED row gives the operator something terminal.
        ESP_LOGW(TAG, "FW_DEPLOY_BEGIN orderList empty — emitting synthetic FAILED row");
        std::vector<astros_fw_deploy_result_t> failures;
        failures.push_back({"unknown", "FAILED", "", "empty_order_list"});
        AstrOs_SerialMsgHandler.sendFwDeployDone(deployMsgId_, deployTransferId_, failures);
        return;
    }

    nextOrderIdx_ = 0;
    results_.clear();
    results_.reserve(orderList_.size());
    active_.store(true);

    ESP_LOGI(TAG, "FW_DEPLOY_BEGIN: transferId=%s targets=%zu", deployTransferId_.c_str(), orderList_.size());

    startNextPadawan();
}
void OtaForwarder::handleBeginAck(queue_ota_forwarder_msg_t &msg)
{
    if (phase_ != Phase::AWAITING_BEGIN_ACK)
    {
        ESP_LOGW(TAG, "Spurious OTA_BEGIN_ACK while phase=%d (xferId=%u); dropping", (int)phase_, msg.begin_ack.xferId);
        return;
    }
    auto r = bulk_.onBeginAck(msg.begin_ack.xferId);
    if (r.decision != AstrOsBulkTransport::BeginAckResult::Decision::OK)
    {
        ESP_LOGW(TAG, "BulkSender::onBeginAck rejected decision=%d (xferId=%u); waiting on timeout", (int)r.decision,
                 msg.begin_ack.xferId);
        return;
    }
    beginAckTimerStop();
    phase_ = Phase::STREAMING;
    // Kick a tick immediately so the first send window starts draining
    // before the 50 ms tick cadence catches up.
    handleTick();
}
void OtaForwarder::handleBeginNak(queue_ota_forwarder_msg_t &msg)
{
    if (phase_ != Phase::AWAITING_BEGIN_ACK)
    {
        ESP_LOGW(TAG, "Spurious OTA_BEGIN_NAK while phase=%d (xferId=%u reason=%u); dropping", (int)phase_,
                 msg.begin_nak.xferId, msg.begin_nak.reason);
        return;
    }

    // Sentinel xferId=0xFF + reason=0xFF means "begin ack timeout fired".
    if (msg.begin_nak.xferId == 0xFF && msg.begin_nak.reason == 0xFF)
    {
        ESP_LOGW(TAG, "OTA_BEGIN_ACK timeout for %s after 2s; abandoning", currentControllerId_.c_str());
        abortCurrentPadawan("begin_ack_timeout");
        return;
    }

    ESP_LOGW(TAG, "OTA_BEGIN_NAK from %s reason=%u; abandoning padawan", currentControllerId_.c_str(),
             msg.begin_nak.reason);
    std::string reasonStr = "begin_nak_" + std::to_string(msg.begin_nak.reason);
    abortCurrentPadawan(reasonStr);
}
void OtaForwarder::handleDataAck(queue_ota_forwarder_msg_t &msg)
{
    if (phase_ != Phase::STREAMING && phase_ != Phase::AWAITING_END_ACK)
    {
        ESP_LOGW(TAG, "Spurious OTA_DATA_ACK while phase=%d (xferId=%u); dropping", (int)phase_, msg.data_ack.xferId);
        return;
    }
    auto r = bulk_.onDataAck(msg.data_ack.xferId, msg.data_ack.highestContiguousSeq);
    switch (r.decision)
    {
    case AstrOsBulkTransport::AckResult::Decision::OK:
        break;
    case AstrOsBulkTransport::AckResult::Decision::STALE:
        ESP_LOGD(TAG, "Stale ACK cumulativeSeq=%u — ignoring", msg.data_ack.highestContiguousSeq);
        return;
    case AstrOsBulkTransport::AckResult::Decision::OUT_OF_RANGE:
        // Log distinctly so a peer-ahead-of-sender mistake is recognizable.
        ESP_LOGW(TAG,
                 "OTA_DATA_ACK OUT_OF_RANGE from %s xferId=%u cumulativeSeq=%u (peer ahead of "
                 "sender) — ignoring",
                 currentControllerId_.c_str(), msg.data_ack.xferId, msg.data_ack.highestContiguousSeq);
        return;
    default:
        ESP_LOGW(TAG, "OTA_DATA_ACK rejected decision=%d cumulativeSeq=%u — ignoring", (int)r.decision,
                 msg.data_ack.highestContiguousSeq);
        return;
    }

    // Use highestContiguousSeq (watermark) for "have we received everything?"
    // rather than newlyConfirmedCount (which blends explicit + implicit ACKs).
    if (msg.data_ack.highestContiguousSeq + 1 >= firmwareTotalChunks_ && phase_ == Phase::STREAMING)
    {
        // All chunks confirmed — time to send OTA_END.
        emitOtaEndFrame();
        phase_ = Phase::AWAITING_END_ACK;
        endAckTimerStart();
        return;
    }

    // Otherwise, the freed in-flight slots let us send more.
    streamDrain(static_cast<uint64_t>(esp_timer_get_time() / 1000));
}
void OtaForwarder::handleDataNak(queue_ota_forwarder_msg_t &msg)
{
    if (phase_ != Phase::STREAMING)
    {
        ESP_LOGW(TAG, "Spurious OTA_DATA_NAK while phase=%d (xferId=%u); dropping", (int)phase_, msg.data_nak.xferId);
        return;
    }
    auto r = bulk_.onDataNak(msg.data_nak.xferId, msg.data_nak.nextExpectedSeq,
                             static_cast<AstrOsBulkTransport::NakReason>(msg.data_nak.reason));
    switch (r.decision)
    {
    case AstrOsBulkTransport::NakResult::Decision::OK:
        break;
    case AstrOsBulkTransport::NakResult::Decision::OUT_OF_RANGE:
        ESP_LOGW(TAG,
                 "OTA_DATA_NAK OUT_OF_RANGE from %s xferId=%u nextExpectedSeq=%u (peer NAK ahead "
                 "of sender) — ignoring",
                 currentControllerId_.c_str(), msg.data_nak.xferId, msg.data_nak.nextExpectedSeq);
        return;
    default:
        ESP_LOGW(TAG, "OTA_DATA_NAK rejected decision=%d — ignoring", (int)r.decision);
        return;
    }
    // The NAK rewinds nextSeqToSend_; the next tick or streamDrain will
    // re-emit from there.
    streamDrain(static_cast<uint64_t>(esp_timer_get_time() / 1000));
}
void OtaForwarder::handleEndAck(queue_ota_forwarder_msg_t &msg)
{
    if (phase_ != Phase::AWAITING_END_ACK)
    {
        ESP_LOGW(TAG, "Spurious OTA_END_ACK while phase=%d (xferId=%u status=%u); dropping", (int)phase_,
                 msg.end_ack.xferId, msg.end_ack.status);
        return;
    }
    endAckTimerStop();
    tickTimerStop();

    // Sentinel xferId=0xFF + status=0xFF means END_ACK timeout fired.
    if (msg.end_ack.xferId == 0xFF && msg.end_ack.status == 0xFF)
    {
        ESP_LOGW(TAG, "OTA_END_ACK timeout for %s after 5s; abandoning", currentControllerId_.c_str());
        abortCurrentPadawan("end_ack_timeout");
        return;
    }

    auto endResult = bulk_.onEndAck(msg.end_ack.xferId, static_cast<OtaEndStatus>(msg.end_ack.status));
    switch (endResult.decision)
    {
    case AstrOsBulkTransport::EndAckResult::Decision::DONE_OK:
        ESP_LOGI(TAG, "Transfer to %s OK", currentControllerId_.c_str());
        completeCurrentPadawan();
        return;
    case AstrOsBulkTransport::EndAckResult::Decision::ABANDONED:
        ESP_LOGW(TAG, "Transfer to %s ABANDONED (status=%u)", currentControllerId_.c_str(), msg.end_ack.status);
        {
            std::string reason = (msg.end_ack.status == static_cast<uint8_t>(OtaEndStatus::HASH_MISMATCH))
                                     ? "hash_mismatch"
                                     : "write_error";
            abortCurrentPadawan(reason);
        }
        return;
    case AstrOsBulkTransport::EndAckResult::Decision::PREMATURE:
        ESP_LOGW(TAG, "OTA_END_ACK PREMATURE (sender state machine internal); abandoning");
        abortCurrentPadawan("premature_end_ack");
        return;
    default:
        ESP_LOGW(TAG, "OTA_END_ACK rejected decision=%d — abandoning", (int)endResult.decision);
        abortCurrentPadawan("end_ack_rejected");
        return;
    }
}
void OtaForwarder::handleTick()
{
    // tick(count=0, abandon=false) is indistinguishable from "not streaming";
    // consult status() separately so we skip ticks while idle.
    auto status = bulk_.status();
    if (status != AstrOsBulkTransport::BulkSender::Status::STREAMING &&
        status != AstrOsBulkTransport::BulkSender::Status::AWAITING_BEGIN_ACK)
    {
        // BulkSender isn't in a state where tick has work; this commonly
        // means we're between transfers or in a terminal state. Skip.
        return;
    }
    if (phase_ != Phase::STREAMING)
    {
        // We're in AWAITING_BEGIN_ACK or AWAITING_END_ACK; tick has no
        // role here (begin/end timeouts are separate timers).
        return;
    }

    uint64_t nowMs = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    auto tr = bulk_.tick(nowMs);

    // Check abandon BEFORE iterating retransmitSeqs — TickResult sets both
    // independently and abandon is the terminal signal.
    if (tr.abandon)
    {
        ESP_LOGW(TAG, "BulkSender abandoned (retry count exceeded) for %s; recording FAILED",
                 currentControllerId_.c_str());
        abortCurrentPadawan("data_retry_exceeded");
        return;
    }

    // Emit retransmits BEFORE pulling new chunks via streamDrain. BulkSender
    // doesn't rewind nextSeqToSend_ when tick triggers retransmits, so a
    // drain-first loop would silently skip them.
    for (uint8_t i = 0; i < tr.count; i++)
    {
        const uint32_t seq = tr.retransmitSeqs[i];
        const uint32_t offset = seq * kChunkSize;
        uint32_t expectedLen = kChunkSize;
        if (offset + kChunkSize > firmwareTotalSize_)
        {
            expectedLen = firmwareTotalSize_ - offset;
        }

        uint8_t payloadBuf[sizeof(OtaDataHeader) + kChunkSize];
        OtaDataHeader hdr{};
        hdr.xferId = currentXferId_;
        hdr.seq = seq;
        hdr.payloadLen = static_cast<uint16_t>(expectedLen);

        std::memcpy(payloadBuf, &hdr, sizeof(hdr));
        if (std::fseek(firmwareFile_, offset, SEEK_SET) != 0 ||
            std::fread(payloadBuf + sizeof(hdr), 1, expectedLen, firmwareFile_) != expectedLen)
        {
            ESP_LOGE(TAG, "Failed to re-read seq=%u for retransmit", seq);
            abortCurrentPadawan("firmware_read_short");
            return;
        }

        uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payloadBuf, sizeof(hdr) + expectedLen);
        std::memcpy(payloadBuf + offsetof(OtaDataHeader, crc16), &crc, sizeof(crc));

        esp_err_t err = AstrOs_EspNow.sendOtaFrame(currentPadawanMac_, AstrOsPacketType::OTA_DATA, payloadBuf,
                                                   sizeof(hdr) + expectedLen);
        if (err != ESP_OK)
        {
            // Mirror streamDrain's send-error log. Don't bail — the next
            // tick will retransmit any unACKed seqs anyway; this keeps
            // the bench log surfacing transient ESP-NOW failures so
            // abandon-counter trips are diagnosable.
            ESP_LOGW(TAG, "OTA_DATA retransmit seq=%u sendOtaFrame returned %s", seq, esp_err_to_name(err));
        }
    }

    // After retransmits, drain new chunks until WINDOW_FULL / ALL_SENT.
    streamDrain(nowMs);
}

void OtaForwarder::startNextPadawan()
{
    // Skip the "master" entry — self-flash isn't wired yet; record FAILED.
    while (nextOrderIdx_ < orderList_.size() && orderList_[nextOrderIdx_] == "master")
    {
        results_.push_back({orderList_[nextOrderIdx_], PadawanStatus::FAILED, "", "master_self_flash_pending"});
        nextOrderIdx_++;
    }

    if (nextOrderIdx_ >= orderList_.size())
    {
        emitDeployDoneAndReset();
        return;
    }

    currentControllerId_ = orderList_[nextOrderIdx_];

    if (!resolveControllerMac(currentControllerId_, currentPadawanMac_))
    {
        ESP_LOGW(TAG, "Controller %s not registered as a peer; recording FAILED", currentControllerId_.c_str());
        results_.push_back({currentControllerId_, PadawanStatus::FAILED, "", "unknown_peer"});
        nextOrderIdx_++;
        startNextPadawan();
        return;
    }

    auto firmwarePathOpt = AstrOs_OtaReceiver.getLastFirmwarePath();
    if (!firmwarePathOpt.has_value())
    {
        ESP_LOGW(TAG, "No staged firmware (getLastFirmwarePath empty) — all-FAILED");
        // Apply no_firmware to the remaining targets.
        while (nextOrderIdx_ < orderList_.size())
        {
            results_.push_back({orderList_[nextOrderIdx_], PadawanStatus::FAILED, "", "no_firmware"});
            nextOrderIdx_++;
        }
        emitDeployDoneAndReset();
        return;
    }

    const std::string &firmwarePath = *firmwarePathOpt;
    firmwareFile_ = std::fopen(firmwarePath.c_str(), "rb");
    if (firmwareFile_ == nullptr)
    {
        ESP_LOGE(TAG, "fopen(%s) failed", firmwarePath.c_str());
        results_.push_back({currentControllerId_, PadawanStatus::FAILED, "", "firmware_open_failed"});
        nextOrderIdx_++;
        startNextPadawan();
        return;
    }

    struct stat st;
    if (stat(firmwarePath.c_str(), &st) != 0)
    {
        ESP_LOGE(TAG, "stat(%s) failed", firmwarePath.c_str());
        std::fclose(firmwareFile_);
        firmwareFile_ = nullptr;
        results_.push_back({currentControllerId_, PadawanStatus::FAILED, "", "firmware_stat_failed"});
        nextOrderIdx_++;
        startNextPadawan();
        return;
    }
    firmwareTotalSize_ = static_cast<uint32_t>(st.st_size);
    firmwareTotalChunks_ = (firmwareTotalSize_ + kChunkSize - 1) / kChunkSize;

    // Zero-byte firmware would surface as the generic "begin_rejected"
    // from BulkSender::begin(totalChunks=0); catch it here so the
    // operator sees the actual cause.
    if (firmwareTotalChunks_ == 0)
    {
        ESP_LOGE(TAG, "Firmware file is empty (size=0); abandoning padawan");
        std::fclose(firmwareFile_);
        firmwareFile_ = nullptr;
        results_.push_back({currentControllerId_, PadawanStatus::FAILED, "", "firmware_empty"});
        nextOrderIdx_++;
        startNextPadawan();
        return;
    }

    // Compute SHA-256 of the file (forensic-grade defensive check; the
    // padawan also verifies). One-shot at file open; ships in the
    // OTA_BEGIN frame's sha256Expected field. 512 B buffer keeps stack
    // pressure low on the 4096 B task stack — SHA-256 throughput is
    // dominated by the 64 B per-block transform, not the I/O buffer.
    AstrOsSha256Ctx shaCtx;
    AstrOsSha256_init(&shaCtx);
    uint8_t buf[512];
    while (size_t r = std::fread(buf, 1, sizeof(buf), firmwareFile_))
    {
        AstrOsSha256_update(&shaCtx, buf, r);
    }
    // fread returning 0 ambiguates EOF vs error; check ferror so a short
    // SD read doesn't ship a SHA computed over a truncated file (which
    // the padawan would report as HASH_MISMATCH for what is really a
    // master-side I/O fault).
    if (std::ferror(firmwareFile_))
    {
        ESP_LOGE(TAG, "fread error during SHA pass; abandoning padawan");
        std::fclose(firmwareFile_);
        firmwareFile_ = nullptr;
        results_.push_back({currentControllerId_, PadawanStatus::FAILED, "", "firmware_read_failed"});
        nextOrderIdx_++;
        startNextPadawan();
        return;
    }
    AstrOsSha256_final(&shaCtx, firmwareSha256_);

    // fseek(SEEK_SET) over rewind(): rewind silently swallows errors. A
    // bad seek here would burn the full chunk budget shipping wrong
    // bytes before the padawan's SHA mismatch catches it — fail fast.
    if (std::fseek(firmwareFile_, 0, SEEK_SET) != 0)
    {
        ESP_LOGE(TAG, "fseek(0) failed after SHA pass");
        std::fclose(firmwareFile_);
        firmwareFile_ = nullptr;
        results_.push_back({currentControllerId_, PadawanStatus::FAILED, "", "firmware_seek_failed"});
        nextOrderIdx_++;
        startNextPadawan();
        return;
    }

    // Bounded by ESPNOW_PEER_LIMIT=10 today; the uint8_t cast would roll
    // over to 0 (the "no xfer in progress" sentinel) at idx 255.
    currentXferId_ = static_cast<uint8_t>(nextOrderIdx_ + 1);

    auto br = bulk_.begin(currentXferId_, firmwareTotalChunks_, kChunkSize, kWindowSize, kAckTimeoutMs, kMaxRetries);
    if (!br.valid)
    {
        ESP_LOGE(TAG, "BulkSender::begin rejected reason=%d", (int)br.reason);
        std::fclose(firmwareFile_);
        firmwareFile_ = nullptr;
        results_.push_back({currentControllerId_, PadawanStatus::FAILED, "", "begin_rejected"});
        nextOrderIdx_++;
        startNextPadawan();
        return;
    }

    ESP_LOGI(TAG, "Starting transfer to %s (xferId=%u, chunks=%u, size=%u)", currentControllerId_.c_str(),
             currentXferId_, firmwareTotalChunks_, firmwareTotalSize_);

    phase_ = Phase::AWAITING_BEGIN_ACK;
    emitOtaBeginFrame();
    beginAckTimerStart();
    tickTimerStart();
}
void OtaForwarder::abortCurrentPadawan(const std::string &reason)
{
    beginAckTimerStop();
    endAckTimerStop();
    tickTimerStop();
    bulk_.reset();
    if (firmwareFile_)
    {
        std::fclose(firmwareFile_);
        firmwareFile_ = nullptr;
    }
    results_.push_back({currentControllerId_, PadawanStatus::FAILED, "", reason});
    currentControllerId_.clear();
    phase_ = Phase::BETWEEN_PADAWANS;
    nextOrderIdx_++;
    startNextPadawan();
}
void OtaForwarder::completeCurrentPadawan()
{
    beginAckTimerStop();
    endAckTimerStop();
    tickTimerStop();
    bulk_.reset();
    if (firmwareFile_)
    {
        std::fclose(firmwareFile_);
        firmwareFile_ = nullptr;
    }
    results_.push_back({currentControllerId_, PadawanStatus::OK, "", ""});
    currentControllerId_.clear();
    phase_ = Phase::BETWEEN_PADAWANS;
    nextOrderIdx_++;
    startNextPadawan();
}
void OtaForwarder::emitDeployDoneAndReset()
{
    ESP_LOGI(TAG, "FW_DEPLOY_DONE: %zu targets, transferId=%s", results_.size(), deployTransferId_.c_str());
    // Mechanical PadawanResult -> astros_fw_deploy_result_t conversion at
    // the wire boundary; the two types deliberately mirror each other so
    // the forwarder's internal type can evolve independently.
    std::vector<astros_fw_deploy_result_t> wire;
    wire.reserve(results_.size());
    for (const auto &r : results_)
    {
        const char *statusStr;
        std::string wireError = r.errorOrEmpty;
        switch (r.status)
        {
        case PadawanStatus::OK:
            statusStr = "OK";
            break;
        case PadawanStatus::FAILED:
            statusStr = "FAILED";
            break;
        default:
            // If a new enum value lands and emitDeployDoneAndReset isn't
            // updated, fail loudly on both master AND wire so the operator
            // sees the bug from the server side instead of just the master
            // log.
            ESP_LOGE(TAG, "Unmapped PadawanStatus=%d; defaulting to FAILED", (int)r.status);
            statusStr = "FAILED";
            wireError = "unmapped_status_" + std::to_string((int)r.status);
            break;
        }
        wire.push_back({r.controllerId, statusStr, r.finalVersion, wireError});
    }
    AstrOs_SerialMsgHandler.sendFwDeployDone(deployMsgId_, deployTransferId_, wire);

    deployMsgId_.clear();
    deployTransferId_.clear();
    orderList_.clear();
    nextOrderIdx_ = 0;
    results_.clear();
    phase_ = Phase::IDLE;
    active_.store(false);
}

void OtaForwarder::postTimeoutSentinel(ota_forwarder_msg_kind_t kind, const char *site)
{
    // Uniform null-check so callers don't all have to guard. xQueueSend on a
    // null handle returns errQUEUE_FULL with no diagnostic — the explicit
    // check below avoids "queue full" misdiagnosis when the real cause is
    // "queue not initialized."
    if (otaForwarderQueue_ == nullptr)
    {
        ESP_LOGE(TAG, "%s: otaForwarderQueue not initialized; sentinel dropped", site);
        return;
    }

    queue_ota_forwarder_msg_t m{};
    m.kind = kind;
    // Sentinel bytes are wire-impossible (xferId is 1..ESPNOW_PEER_LIMIT=10).
    switch (kind)
    {
    case OTA_FWD_BEGIN_NAK:
        m.begin_nak.xferId = 0xFF;
        m.begin_nak.reason = 0xFF;
        break;
    case OTA_FWD_END_ACK:
        m.end_ack.xferId = 0xFF;
        m.end_ack.status = 0xFF;
        break;
    default:
        ESP_LOGE(TAG, "postTimeoutSentinel: unsupported kind %d from %s", (int)kind, site);
        return;
    }
    if (xQueueSend(otaForwarderQueue_, &m, 0) != pdTRUE)
    {
        ESP_LOGE(TAG, "%s: otaForwarderQueue full; sentinel dropped — forwarder may hang", site);
    }
}

void OtaForwarder::emitOtaBeginFrame()
{
    OtaBeginPayload payload{};
    payload.xferId = currentXferId_;
    payload.totalSize = firmwareTotalSize_;
    payload.chunkSize = kChunkSize;
    payload.totalChunks = firmwareTotalChunks_;
    std::memcpy(payload.sha256Expected, firmwareSha256_, 32);
    payload.flags = 0;

    esp_err_t err = AstrOs_EspNow.sendOtaFrame(currentPadawanMac_, AstrOsPacketType::OTA_BEGIN,
                                               reinterpret_cast<const uint8_t *>(&payload), sizeof(payload));
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "OTA_BEGIN sendOtaFrame returned %s; posting timeout sentinel immediately", esp_err_to_name(err));
        // Padawan never saw the frame — fail fast instead of waiting 2 s.
        postTimeoutSentinel(OTA_FWD_BEGIN_NAK, "emitOtaBeginFrame");
    }
}
void OtaForwarder::emitOtaEndFrame()
{
    OtaEndPayload payload{};
    payload.xferId = currentXferId_;
    payload.totalChunksSent = firmwareTotalChunks_;
    std::memcpy(payload.sha256Final, firmwareSha256_, 32);

    esp_err_t err = AstrOs_EspNow.sendOtaFrame(currentPadawanMac_, AstrOsPacketType::OTA_END,
                                               reinterpret_cast<const uint8_t *>(&payload), sizeof(payload));
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "OTA_END sendOtaFrame returned %s; posting timeout sentinel immediately", esp_err_to_name(err));
        // Padawan never saw the frame — fail fast instead of waiting 5 s.
        postTimeoutSentinel(OTA_FWD_END_ACK, "emitOtaEndFrame");
    }
}
void OtaForwarder::streamDrain(uint64_t nowMs)
{
    for (;;)
    {
        auto sr = bulk_.nextChunkToSend(nowMs);
        if (sr.decision == AstrOsBulkTransport::SendResult::Decision::SEND)
        {
            // Read the chunk from disk. Chunks are chunkSize except
            // possibly the last one.
            const uint32_t seq = sr.seq;
            const uint32_t offset = seq * kChunkSize;
            uint32_t expectedLen = kChunkSize;
            if (offset + kChunkSize > firmwareTotalSize_)
            {
                expectedLen = firmwareTotalSize_ - offset;
            }

            uint8_t payloadBuf[sizeof(OtaDataHeader) + kChunkSize];
            OtaDataHeader hdr{};
            hdr.xferId = currentXferId_;
            hdr.seq = seq;
            hdr.payloadLen = static_cast<uint16_t>(expectedLen);
            // CRC computed after the bytes are loaded below.

            std::memcpy(payloadBuf, &hdr, sizeof(hdr));
            if (std::fseek(firmwareFile_, offset, SEEK_SET) != 0)
            {
                ESP_LOGE(TAG, "fseek(%u) failed mid-transfer; abandoning", offset);
                abortCurrentPadawan("firmware_seek_failed");
                return;
            }
            size_t read = std::fread(payloadBuf + sizeof(hdr), 1, expectedLen, firmwareFile_);
            if (read != expectedLen)
            {
                ESP_LOGE(TAG, "fread(%u) returned %zu mid-transfer; abandoning", expectedLen, read);
                abortCurrentPadawan("firmware_read_short");
                return;
            }

            // CRC-16/CCITT-FALSE over the entire OtaDataHeader bytes +
            // payload bytes per the M1 wire spec (OtaWirePayloads.hpp:65 —
            // "[xferId..end of firmware-bytes]"). The crc16 field inside
            // the header is zero at this point (zero-init on hdr{}); the
            // computed CRC is patched in below. M4's padawan verifier
            // must zero the crc16 field before recomputing.
            //
            // NOTE: this span differs from the serial-path BulkReceiver,
            // which checks CRC over payload only — the two transports
            // deliberately use different CRC contracts.
            uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payloadBuf, sizeof(hdr) + expectedLen);
            std::memcpy(payloadBuf + offsetof(OtaDataHeader, crc16), &crc, sizeof(crc));

            esp_err_t err = AstrOs_EspNow.sendOtaFrame(currentPadawanMac_, AstrOsPacketType::OTA_DATA, payloadBuf,
                                                       sizeof(hdr) + expectedLen);
            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "OTA_DATA seq=%u sendOtaFrame returned %s; tick will retry", seq, esp_err_to_name(err));
                // Don't abort here — tick-based retransmit will catch it.
                return;
            }
            continue;
        }

        // ALL_SENT: OTA_END fires from handleDataAck when the confirmed
        // watermark reaches totalChunks, not from here. WINDOW_FULL /
        // NOT_STREAMING: stop draining for now.
        if (sr.decision == AstrOsBulkTransport::SendResult::Decision::ALL_SENT)
        {
            return;
        }
        return; // WINDOW_FULL or NOT_STREAMING
    }
}

void OtaForwarder::tickTimerStart()
{
    if (tickTimer_)
    {
        esp_timer_start_periodic(tickTimer_, kTickPeriodUs);
    }
}
void OtaForwarder::tickTimerStop()
{
    if (tickTimer_)
    {
        esp_timer_stop(tickTimer_);
    }
}
void OtaForwarder::beginAckTimerStart()
{
    if (beginAckTimer_)
    {
        esp_timer_start_once(beginAckTimer_, kBeginAckTimeoutUs);
    }
}
void OtaForwarder::beginAckTimerStop()
{
    if (beginAckTimer_)
    {
        esp_timer_stop(beginAckTimer_);
    }
}
void OtaForwarder::endAckTimerStart()
{
    if (endAckTimer_)
    {
        esp_timer_start_once(endAckTimer_, kEndAckTimeoutUs);
    }
}
void OtaForwarder::endAckTimerStop()
{
    if (endAckTimer_)
    {
        esp_timer_stop(endAckTimer_);
    }
}

void OtaForwarder::tickTimerCb(void *arg)
{
    OtaForwarder *self = static_cast<OtaForwarder *>(arg);
    if (!self || !self->otaForwarderQueue_)
    {
        return;
    }
    queue_ota_forwarder_msg_t m{};
    m.kind = OTA_FWD_TICK;
    // Best-effort post; if the queue is full the next tick will catch up.
    xQueueSend(self->otaForwarderQueue_, &m, 0);
}

void OtaForwarder::beginAckTimerCb(void *arg)
{
    OtaForwarder *self = static_cast<OtaForwarder *>(arg);
    if (!self || !self->otaForwarderQueue_)
    {
        return;
    }
    self->postTimeoutSentinel(OTA_FWD_BEGIN_NAK, "beginAckTimerCb");
}

void OtaForwarder::endAckTimerCb(void *arg)
{
    OtaForwarder *self = static_cast<OtaForwarder *>(arg);
    if (!self || !self->otaForwarderQueue_)
    {
        return;
    }
    self->postTimeoutSentinel(OTA_FWD_END_ACK, "endAckTimerCb");
}

bool OtaForwarder::resolveControllerMac(const std::string &controllerId, uint8_t outMac[6]) const
{
    // Linear scan over AstrOs_EspNow.getPeers() — list is bounded by
    // ESPNOW_PEER_LIMIT (10), so the cost is negligible. getPeers
    // internally acquires the peer mutex and returns a vector copy, so
    // the read is thread-safe.
    auto peers = AstrOs_EspNow.getPeers();
    for (const auto &p : peers)
    {
        if (controllerId == p.name)
        {
            std::memcpy(outMac, p.mac_addr, 6);
            return true;
        }
    }
    return false;
}

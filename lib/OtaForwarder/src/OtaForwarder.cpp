#include "OtaForwarder.hpp"

#include <AstrOsEspAppDescParser.hpp>
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
    for (esp_timer_handle_t *t : {&tickTimer_, &beginAckTimer_, &endAckTimer_, &statsTimer_, &flashResultTimer_})
    {
        if (*t)
        {
            esp_timer_stop(*t);
            esp_timer_delete(*t);
            *t = nullptr;
        }
    }
    if (versionConfirmTimer_ != nullptr)
    {
        esp_timer_stop(versionConfirmTimer_);
        esp_timer_delete(versionConfirmTimer_);
        versionConfirmTimer_ = nullptr;
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

    esp_timer_create_args_t statsArgs = {
        .callback = &OtaForwarder::statsTimerCb,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ota_fwd_stats",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&statsArgs, &statsTimer_));

    esp_timer_create_args_t flashResultArgs = {
        .callback = &OtaForwarder::flashResultTimerCb,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ota_fwd_flashres",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&flashResultArgs, &flashResultTimer_));

    esp_timer_create_args_t versionConfirmArgs = {
        .callback = &OtaForwarder::versionConfirmTimerCb,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ota_fwd_versionconf",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&versionConfirmArgs, &versionConfirmTimer_));
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
    case OTA_FWD_STATS_FIRE:
        handleStatsFire();
        break;
    case OTA_FWD_FLASH_RESULT:
        handleFlashResult(msg);
        break;
    case OTA_FWD_FLASH_RESULT_TIMEOUT:
        handleFlashResultTimeout();
        break;
    case OTA_FWD_VERSION_CONFIRM_TIMEOUT:
        handleVersionConfirmTimeout();
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

    // Cap the order list at kMaxOrderListSize so the per-padawan xferId
    // derivation (nextOrderIdx_ + 1) can't wrap into the sentinel range
    // (0 = "no xfer in progress", 0xFF = "timeout sentinel"). An
    // oversized list almost certainly indicates a malformed or hostile
    // FW_DEPLOY_BEGIN; reject the whole deploy with one FAILED row per
    // submitted target so JobLock terminates cleanly.
    if (orderList_.size() > kMaxOrderListSize)
    {
        ESP_LOGW(TAG, "FW_DEPLOY_BEGIN orderList size %zu exceeds cap %zu — rejecting", orderList_.size(),
                 (size_t)kMaxOrderListSize);
        std::vector<astros_fw_deploy_result_t> failures;
        failures.reserve(orderList_.size());
        for (const auto &id : orderList_)
        {
            failures.push_back({id, "FAILED", "", "order_list_too_large"});
        }
        AstrOs_SerialMsgHandler.sendFwDeployDone(deployMsgId_, deployTransferId_, failures);
        orderList_.clear();
        return;
    }

    nextOrderIdx_ = 0;
    results_.clear();
    results_.reserve(orderList_.size());
    active_.store(true);
    wireBusy_.store(true);

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
    if (!isFromCurrentPadawan(msg.begin_ack.srcMac))
    {
        ESP_LOGW(TAG, "OTA_BEGIN_ACK from unexpected peer (xferId=%u); dropping", msg.begin_ack.xferId);
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
    tickTimerStart();
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
    // Check BEFORE the srcMac validation: the timer-cb sentinel has an
    // all-zero srcMac (zero-init), so srcMac check would reject it.
    if (msg.begin_nak.xferId == 0xFF && msg.begin_nak.reason == 0xFF)
    {
        ESP_LOGW(TAG, "OTA_BEGIN_ACK timeout for %s after 5s; abandoning", currentControllerId_.c_str());
        abortCurrentPadawan("begin_ack_timeout");
        return;
    }

    if (!isFromCurrentPadawan(msg.begin_nak.srcMac))
    {
        ESP_LOGW(TAG, "OTA_BEGIN_NAK from unexpected peer (xferId=%u reason=%u); dropping", msg.begin_nak.xferId,
                 msg.begin_nak.reason);
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
    if (!isFromCurrentPadawan(msg.data_ack.srcMac))
    {
        ESP_LOGW(TAG, "OTA_DATA_ACK from unexpected peer (xferId=%u cumulativeSeq=%u); dropping", msg.data_ack.xferId,
                 msg.data_ack.highestContiguousSeq);
        return;
    }
    auto r = bulk_.onDataAck(msg.data_ack.xferId, msg.data_ack.highestContiguousSeq);
    switch (r.decision)
    {
    case AstrOsBulkTransport::AckResult::Decision::OK:
        // Update stats cursor — the wire's cumulative ACK is authoritative.
        statsHighestAckedSeq_ = msg.data_ack.highestContiguousSeq;
        statsAnyAcked_ = true;
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
        // Stop the tick timer: AWAITING_END_ACK has no streaming work, so
        // continued ticks would just enqueue no-ops and risk crowding the
        // end-ack timeout sentinel out of the queue.
        tickTimerStop();
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
    if (!isFromCurrentPadawan(msg.data_nak.srcMac))
    {
        ESP_LOGW(TAG, "OTA_DATA_NAK from unexpected peer (xferId=%u nextExpectedSeq=%u); dropping", msg.data_nak.xferId,
                 msg.data_nak.nextExpectedSeq);
        return;
    }
    statsNaksRecvCount_++;
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

    // Sentinel xferId=0xFF + status=0xFF means END_ACK timeout fired.
    // Check BEFORE the srcMac validation: the timer-cb sentinel has an
    // all-zero srcMac (zero-init), so srcMac check would reject it.
    if (msg.end_ack.xferId == 0xFF && msg.end_ack.status == 0xFF)
    {
        endAckTimerStop();
        tickTimerStop();
        ESP_LOGW(TAG, "OTA_END_ACK timeout for %s after 5s; abandoning", currentControllerId_.c_str());
        abortCurrentPadawan("end_ack_timeout");
        return;
    }

    if (!isFromCurrentPadawan(msg.end_ack.srcMac))
    {
        // Don't stop the timer — the real padawan might still respond, or
        // the timeout will resolve the wait correctly.
        ESP_LOGW(TAG, "OTA_END_ACK from unexpected peer (xferId=%u status=%u); dropping", msg.end_ack.xferId,
                 msg.end_ack.status);
        return;
    }

    endAckTimerStop();
    tickTimerStop();

    auto endResult = bulk_.onEndAck(msg.end_ack.xferId, static_cast<OtaEndStatus>(msg.end_ack.status));
    switch (endResult.decision)
    {
    case AstrOsBulkTransport::EndAckResult::Decision::DONE_OK:
        ESP_LOGI(TAG, "Transfer to %s verified; awaiting flash result", currentControllerId_.c_str());
        // FLASHING fires on END_ACK OK arrival. The flash row stays visible
        // for the duration of the padawan's pre-flash delay before
        // OTA_FLASH_RESULT lands.
        AstrOs_SerialMsgHandler.sendFwProgress(deployTransferId_, currentControllerId_, "FLASHING", firmwareTotalSize_,
                                               firmwareTotalSize_, "");
        // On OK, the padawan has verified but not yet committed. Enter the
        // AWAITING_FLASH_RESULT phase and wait for OTA_FLASH_RESULT to land.
        // kFlashResultTimeoutUs is the safety bound on padawan misbehavior
        // during the pre-flash delay window.
        // Stop stats timer — counters are frozen at this point; continued
        // 2 s heartbeats with unchanged values would just be log noise.
        statsTimerStop();
        phase_ = Phase::AWAITING_FLASH_RESULT;
        flashResultTimerStart();
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
    // Phase A: drive AWAITING_VERSION_CONFIRMED polling first. handleTick
    // fires every 50 ms regardless of bulk_ state, but the STREAMING-only
    // logic below is the more restrictive caller. Run version-check before
    // the bulk_/STREAMING gates so it actually sees the tick.
    if (phase_ == Phase::AWAITING_VERSION_CONFIRMED)
    {
        checkPeerVersionForCurrentPadawan();
        return;
    }

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
        // Split fseek/fread checks so a seek failure isn't conflated with a
        // short read in FW_DEPLOY_DONE. Reason vocabulary:
        //   firmware_seek_failed — fseek returned non-zero (seek-time fault)
        //   firmware_read_short  — fread returned fewer bytes than requested
        //                          (file changed mid-transfer: truncation,
        //                          unlink, sparse-file weirdness)
        //   firmware_read_failed — ferror() set during the SHA pass at file
        //                          open (SD driver/hardware fault)
        // Different root causes → different operator next-steps. Matches
        // streamDrain's per-check splitting.
        if (std::fseek(firmwareFile_, offset, SEEK_SET) != 0)
        {
            ESP_LOGE(TAG, "fseek(%u) failed for retransmit seq=%u; abandoning", offset, seq);
            abortCurrentPadawan("firmware_seek_failed");
            return;
        }
        size_t read = std::fread(payloadBuf + sizeof(hdr), 1, expectedLen, firmwareFile_);
        if (read != expectedLen)
        {
            ESP_LOGE(TAG, "fread(%u) returned %zu for retransmit seq=%u; abandoning", expectedLen, read, seq);
            abortCurrentPadawan("firmware_read_short");
            return;
        }

        // CRC over payload only — see streamDrain for the contract rationale.
        uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payloadBuf + sizeof(hdr), expectedLen);
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
            statsSendFailCount_++;
        }
        // Don't update statsLastSentSeq_ here — retransmits are by definition
        // of seqs already covered by the high-water mark.
    }

    // After retransmits, drain new chunks until WINDOW_FULL / ALL_SENT.
    streamDrain(nowMs);
}

void OtaForwarder::startNextPadawan()
{
    // Iterative advance through the order list. The order-list length is
    // operator-controlled (no wire-layer bound), so a recursive walk could
    // blow the 4096 B task stack on a long list of unknown_peer / invalid
    // entries. Loop until a valid target is found or a terminal condition
    // (order exhausted / no_firmware) fires.
    while (true)
    {
        // Skip the master self-flash entry — encoded as the all-zero MAC by
        // the Pi-side FW_DEPLOY_BEGIN convention (see the matching
        // "00:00:00:00:00:00" / "master" pair in
        // astros_serial_protocol_tests fixtures). Self-flash isn't wired
        // yet; record FAILED so the deploy advances to the padawans.
        if (nextOrderIdx_ < orderList_.size() && orderList_[nextOrderIdx_] == "00:00:00:00:00:00")
        {
            results_.push_back({orderList_[nextOrderIdx_], PadawanStatus::FAILED, "", "master_self_flash_pending"});
            nextOrderIdx_++;
            continue;
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
            continue;
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
            continue;
        }

        struct stat st;
        if (stat(firmwarePath.c_str(), &st) != 0)
        {
            ESP_LOGE(TAG, "stat(%s) failed", firmwarePath.c_str());
            std::fclose(firmwareFile_);
            firmwareFile_ = nullptr;
            results_.push_back({currentControllerId_, PadawanStatus::FAILED, "", "firmware_stat_failed"});
            nextOrderIdx_++;
            continue;
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
            continue;
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
            continue;
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
            continue;
        }

        // Phase A: read the staged .bin's esp_app_desc_t to learn the expected
        // post-reboot version string. The 80-byte prefix is plenty — the
        // parser only reads bytes 0..79. fseek back to 0 afterward so the
        // streaming send loop starts at the beginning.
        expectedNewVersion_.clear();
        {
            uint8_t prefix[80] = {0};
            if (std::fread(prefix, 1, sizeof(prefix), firmwareFile_) != sizeof(prefix))
            {
                ESP_LOGW(TAG,
                         "Could not read 80-byte prefix from staged .bin (%s); "
                         "version-confirm will fall back to timeout",
                         firmwarePath.c_str());
            }
            else
            {
                auto desc = AstrOsEspAppDescParser::parse(prefix, sizeof(prefix));
                if (desc.ok)
                {
                    expectedNewVersion_ = desc.version;
                    ESP_LOGI(TAG, "Parsed expected new version '%s' from staged .bin", expectedNewVersion_.c_str());
                }
                else
                {
                    ESP_LOGW(TAG,
                             "esp_app_desc parse failed (%s); version-confirm will "
                             "fall back to timeout",
                             desc.error.c_str());
                }
            }
            if (std::fseek(firmwareFile_, 0, SEEK_SET) != 0)
            {
                ESP_LOGE(TAG, "fseek(0) failed on firmware file");
                std::fclose(firmwareFile_);
                firmwareFile_ = nullptr;
                results_.push_back({currentControllerId_, PadawanStatus::FAILED, "", "firmware_seek_failed"});
                nextOrderIdx_++;
                continue;
            }
        }

        // Safe because handleDeployBegin caps orderList_.size() at
        // kMaxOrderListSize (< 0xFE per the static_assert in the header).
        // Yields xferIds 1..kMaxOrderListSize — never 0 ("no xfer") or
        // 0xFF ("timeout sentinel").
        currentXferId_ = static_cast<uint8_t>(nextOrderIdx_ + 1);

        auto br =
            bulk_.begin(currentXferId_, firmwareTotalChunks_, kChunkSize, kWindowSize, kAckTimeoutMs, kMaxRetries);
        if (!br.valid)
        {
            ESP_LOGE(TAG, "BulkSender::begin rejected reason=%d", (int)br.reason);
            std::fclose(firmwareFile_);
            firmwareFile_ = nullptr;
            results_.push_back({currentControllerId_, PadawanStatus::FAILED, "", "begin_rejected"});
            nextOrderIdx_++;
            continue;
        }

        ESP_LOGI(TAG, "Starting transfer to %s (xferId=%u, chunks=%u, size=%u)", currentControllerId_.c_str(),
                 currentXferId_, firmwareTotalChunks_, firmwareTotalSize_);

        // Reset per-padawan stats counters before the first wire activity.
        statsLastSentSeq_ = 0;
        statsHighestAckedSeq_ = 0;
        statsAnyAcked_ = false;
        statsNaksRecvCount_ = 0;
        statsSendFailCount_ = 0;
        lastProgressBytesSent_ = 0;

        phase_ = Phase::AWAITING_BEGIN_ACK;
        wireBusy_.store(true); // wire is active again for this padawan's transfer
        emitOtaBeginFrame();
        beginAckTimerStart();
        statsTimerStart();

        // SENDING at 0 bytes — announces this padawan has entered the transfer
        // pipeline; the operator sees the row go live as soon as OTA_BEGIN is
        // sent (before streaming starts in STREAMING phase).
        AstrOs_SerialMsgHandler.sendFwProgress(deployTransferId_, currentControllerId_, "SENDING",
                                               /*bytesSent=*/0, /*totalBytes=*/firmwareTotalSize_, /*detail=*/"");
        // tickTimer is started only when entering STREAMING (see handleBeginAck);
        // running it during AWAITING_BEGIN_ACK / AWAITING_END_ACK just floods the
        // queue with no-op messages and risks crowding out deadline sentinels.
        return;
    }
}
void OtaForwarder::finishCurrentPadawanAndAdvance()
{
    // Defensive: stop every timer that could still be running. Some of
    // these are no-ops in the normal flow (e.g., endAckTimer already
    // stopped in handleEndAck), but centralizing avoids future drift
    // when a new abort path forgets one.
    beginAckTimerStop();
    endAckTimerStop();
    tickTimerStop();
    statsTimerStop();
    flashResultTimerStop();
    versionConfirmTimerStop();

    bulk_.reset();

    if (firmwareFile_)
    {
        std::fclose(firmwareFile_);
        firmwareFile_ = nullptr;
    }

    currentControllerId_.clear();
    flashResultSpuriousDrops_ = 0;
    phase_ = Phase::BETWEEN_PADAWANS;
    nextOrderIdx_++;
    startNextPadawan();
}
void OtaForwarder::abortCurrentPadawan(const std::string &reason)
{
    results_.push_back({currentControllerId_, PadawanStatus::FAILED, "", reason});
    finishCurrentPadawanAndAdvance();
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
    wireBusy_.store(false);
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
    // Use 0xFF as a local timeout sentinel. Real xferId values are non-zero
    // and derived from the order-list index (+1), so they are bounded by the
    // configured order-list size rather than ESPNOW_PEER_LIMIT; this path
    // relies only on excluding 0 and 0xFF from real wire values.
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
        statsSendFailCount_++;
        // Padawan never saw the frame — fail fast instead of waiting 5 s.
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
        statsSendFailCount_++;
        // Padawan never saw the frame — fail fast instead of waiting 5 s.
        postTimeoutSentinel(OTA_FWD_END_ACK, "emitOtaEndFrame");
        return;
    }

    // VERIFYING fires when OTA_END is sent (not when END_ACK arrives) so the
    // verify row is already visible in the UI while the padawan runs its 3
    // integrity gates and the 2 s pre-flash delay.
    AstrOs_SerialMsgHandler.sendFwProgress(deployTransferId_, currentControllerId_, "VERIFYING", firmwareTotalSize_,
                                           firmwareTotalSize_, "");
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

            // CRC-16/CCITT-FALSE over payload bytes only. Matches what
            // BulkReceiver::onChunk recomputes on the padawan side and the
            // serial-path receiver — both transports share one CRC contract.
            // Header-byte integrity is already covered by ESP-NOW's MAC-layer
            // CRC32 and parseOtaData's field validation.
            uint16_t crc = AstrOsBulkTransport::crc16_ccitt_false(payloadBuf + sizeof(hdr), expectedLen);
            std::memcpy(payloadBuf + offsetof(OtaDataHeader, crc16), &crc, sizeof(crc));

            esp_err_t err = AstrOs_EspNow.sendOtaFrame(currentPadawanMac_, AstrOsPacketType::OTA_DATA, payloadBuf,
                                                       sizeof(hdr) + expectedLen);
            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "OTA_DATA seq=%u sendOtaFrame returned %s; tick will retry", seq, esp_err_to_name(err));
                statsSendFailCount_++;
                // Don't abort here — tick-based retransmit will catch it.
                return;
            }
            // Track highest seq put on wire (monotonic — retransmits don't
            // regress this, so the stat reflects progress, not the most
            // recent retry).
            if (seq > statsLastSentSeq_)
                statsLastSentSeq_ = seq;

            // Emit SENDING FW_PROGRESS every >=5% of firmwareTotalSize_
            // bytes-sent advance. The first-byte emission already fired in
            // startNextPadawan; this picks up from there. Integer math only —
            // no FP in the hot path.
            {
                uint32_t bytesSent = static_cast<uint32_t>(seq + 1) * static_cast<uint32_t>(kChunkSize);
                if (bytesSent > firmwareTotalSize_)
                    bytesSent = firmwareTotalSize_; // cap on last (short) chunk
                uint32_t fivePct = firmwareTotalSize_ / 20;
                if (fivePct == 0)
                    fivePct = 1; // degenerate small-firmware safety
                if (bytesSent >= lastProgressBytesSent_ + fivePct)
                {
                    lastProgressBytesSent_ = bytesSent;
                    AstrOs_SerialMsgHandler.sendFwProgress(deployTransferId_, currentControllerId_, "SENDING",
                                                           bytesSent, firmwareTotalSize_, "");
                }
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

// All three Start helpers use stop-then-start: esp_timer has no native
// restart and start on an already-running timer returns
// ESP_ERR_INVALID_STATE. Stop on an idle timer is a documented no-op,
// so the leading stop is safe regardless of prior state. Capture the
// start return value and log on failure so a silently-dead timer
// (the forwarder would hang in AWAITING_* forever) is diagnosable.
void OtaForwarder::tickTimerStart()
{
    if (!tickTimer_)
        return;
    esp_timer_stop(tickTimer_);
    esp_err_t err = esp_timer_start_periodic(tickTimer_, kTickPeriodUs);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "tickTimerStart: esp_timer_start_periodic failed: %s — streaming will stall",
                 esp_err_to_name(err));
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
    if (!beginAckTimer_)
        return;
    esp_timer_stop(beginAckTimer_);
    esp_err_t err = esp_timer_start_once(beginAckTimer_, kBeginAckTimeoutUs);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "beginAckTimerStart: esp_timer_start_once failed: %s — forwarder may hang in AWAITING_BEGIN_ACK",
                 esp_err_to_name(err));
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
    if (!endAckTimer_)
        return;
    esp_timer_stop(endAckTimer_);
    esp_err_t err = esp_timer_start_once(endAckTimer_, kEndAckTimeoutUs);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "endAckTimerStart: esp_timer_start_once failed: %s — forwarder may hang in AWAITING_END_ACK",
                 esp_err_to_name(err));
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

void OtaForwarder::statsTimerStart()
{
    if (!statsTimer_)
        return;
    esp_timer_stop(statsTimer_);
    esp_err_t err = esp_timer_start_periodic(statsTimer_, kStatsPeriodUs);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "statsTimerStart: esp_timer_start_periodic failed: %s — stats log will be silent",
                 esp_err_to_name(err));
    }
}

void OtaForwarder::statsTimerStop()
{
    if (statsTimer_)
    {
        esp_timer_stop(statsTimer_);
    }
}

void OtaForwarder::statsTimerCb(void *arg)
{
    OtaForwarder *self = static_cast<OtaForwarder *>(arg);
    if (!self || !self->otaForwarderQueue_)
    {
        return;
    }
    queue_ota_forwarder_msg_t m{};
    m.kind = OTA_FWD_STATS_FIRE;
    // Best-effort: missing one stats fire is harmless; next tick catches up.
    xQueueSend(self->otaForwarderQueue_, &m, 0);
}

void OtaForwarder::handleStatsFire()
{
    if (phase_ == Phase::IDLE)
    {
        // Stale fire arriving after the transfer ended; stop the timer to
        // prevent further noise (start/stop wiring guarantees this is rare).
        statsTimerStop();
        return;
    }
    const char *phaseStr = "?";
    switch (phase_)
    {
    case Phase::IDLE:
        phaseStr = "IDLE";
        break;
    case Phase::AWAITING_BEGIN_ACK:
        phaseStr = "AWAITING_BEGIN_ACK";
        break;
    case Phase::STREAMING:
        phaseStr = "STREAMING";
        break;
    case Phase::AWAITING_END_ACK:
        phaseStr = "AWAITING_END_ACK";
        break;
    case Phase::AWAITING_FLASH_RESULT:
        phaseStr = "AWAITING_FLASH_RESULT";
        break;
    case Phase::AWAITING_VERSION_CONFIRMED:
        phaseStr = "AWAITING_VERSION_CONFIRMED";
        break;
    case Phase::BETWEEN_PADAWANS:
        phaseStr = "BETWEEN_PADAWANS";
        break;
    }
    const long long acked = statsAnyAcked_ ? static_cast<long long>(statsHighestAckedSeq_) : -1;
    ESP_LOGI(TAG, "OTA_STATS_TX: xferId=%u seq=%u/%u acked=%lld naks-rx=%u send-fail=%u phase=%s",
             (unsigned)currentXferId_, (unsigned)statsLastSentSeq_, (unsigned)firmwareTotalChunks_, acked,
             (unsigned)statsNaksRecvCount_, (unsigned)statsSendFailCount_, phaseStr);
}

void OtaForwarder::flashResultTimerStart()
{
    if (!flashResultTimer_)
        return;
    esp_timer_stop(flashResultTimer_);
    esp_err_t err = esp_timer_start_once(flashResultTimer_, kFlashResultTimeoutUs);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "flashResultTimerStart: esp_timer_start_once failed: %s — "
                 "no safety net; padawan crash during pre-flash delay would block deploy",
                 esp_err_to_name(err));
    }
}

void OtaForwarder::flashResultTimerStop()
{
    if (flashResultTimer_)
    {
        esp_timer_stop(flashResultTimer_);
    }
}

void OtaForwarder::flashResultTimerCb(void *arg)
{
    OtaForwarder *self = static_cast<OtaForwarder *>(arg);
    if (!self || !self->otaForwarderQueue_)
        return;
    queue_ota_forwarder_msg_t m{};
    m.kind = OTA_FWD_FLASH_RESULT_TIMEOUT;
    // Last-resort recovery for a padawan crash during the pre-flash
    // delay. If this sentinel is dropped, the forwarder hangs in
    // AWAITING_FLASH_RESULT forever — log loudly so the bench operator
    // sees the cause.
    if (xQueueSend(self->otaForwarderQueue_, &m, 0) != pdTRUE)
    {
        ESP_LOGE(TAG, "flashResultTimerCb: otaForwarderQueue full; sentinel dropped — "
                      "forwarder may hang in AWAITING_FLASH_RESULT");
    }
}

void OtaForwarder::versionConfirmTimerCb(void *arg)
{
    OtaForwarder *self = static_cast<OtaForwarder *>(arg);
    if (!self || !self->otaForwarderQueue_)
        return;
    queue_ota_forwarder_msg_t m{};
    m.kind = OTA_FWD_VERSION_CONFIRM_TIMEOUT;
    // Safety bound for a padawan that rebooted but never reported the expected
    // version. If this sentinel is dropped, the forwarder hangs in
    // AWAITING_VERSION_CONFIRMED forever — log loudly so bench logs show cause.
    if (xQueueSend(self->otaForwarderQueue_, &m, 0) != pdTRUE)
    {
        ESP_LOGE(TAG, "versionConfirmTimerCb: otaForwarderQueue full; sentinel dropped — "
                      "forwarder may hang in AWAITING_VERSION_CONFIRMED");
    }
}

void OtaForwarder::handleFlashResult(queue_ota_forwarder_msg_t &msg)
{
    // Rate-limited drop logger: emits on first occurrence and every 10th
    // thereafter. Prevents log saturation under mesh spam.
    auto logSpurious = [this](const char *reason, unsigned xferId)
    {
        if (flashResultSpuriousDrops_ == 0 || flashResultSpuriousDrops_ % 10 == 0)
        {
            ESP_LOGW(TAG, "handleFlashResult: %s (xferId=%u) — dropping (spurious count=%u)", reason, xferId,
                     (unsigned)(flashResultSpuriousDrops_ + 1));
        }
        flashResultSpuriousDrops_++;
    };

    if (phase_ != Phase::AWAITING_FLASH_RESULT)
    {
        logSpurious("phase mismatch", (unsigned)msg.flash_result.xferId);
        return;
    }
    if (msg.flash_result.xferId != currentXferId_)
    {
        logSpurious("xferId mismatch", (unsigned)msg.flash_result.xferId);
        return;
    }
    if (!isFromCurrentPadawan(msg.flash_result.srcMac))
    {
        logSpurious("unexpected peer", (unsigned)msg.flash_result.xferId);
        return;
    }
    flashResultTimerStop();

    OtaFlashStatus status = static_cast<OtaFlashStatus>(msg.flash_result.status);
    std::string wireReason(msg.flash_result.reason, msg.flash_result.reasonLen);

    auto mapped = AstrOsEspNowProtocol::mapOtaFlashStatusToResult(status, wireReason);

    ESP_LOGI(TAG, "Flash result for %s: status=%d reason='%s'", currentControllerId_.c_str(), (int)status,
             mapped.errorReason.c_str());

    if (status == OtaFlashStatus::OK)
    {
        // Padawan reported flash success and is about to reboot. Arm the
        // version-confirm safety timer FIRST — if it fails there is no
        // timeout net and the forwarder would hang, so abort immediately
        // rather than entering AWAITING_VERSION_CONFIRMED.
        if (!versionConfirmTimerStart())
        {
            ESP_LOGE(TAG, "handleFlashResult: versionConfirmTimerStart failed for %s — aborting padawan",
                     currentControllerId_.c_str());
            abortCurrentPadawan("version_confirm_timer_failed");
            return;
        }

        // Don't record SUCCESS yet — wait until heartbeat shows the expected
        // new version. The flash row already lit FLASHING when END_ACK OK
        // landed; now signal the reboot transition for the UI.
        AstrOs_SerialMsgHandler.sendFwProgress(deployTransferId_, currentControllerId_, "REBOOTING", firmwareTotalSize_,
                                               firmwareTotalSize_, "");

        wireBusy_.store(false); // wire idle during AWAITING_VERSION_CONFIRMED; master must poll to observe POLL_ACK
        phase_ = Phase::AWAITING_VERSION_CONFIRMED;
        tickTimerStart(); // re-arm the 50 ms tick (it was stopped after END_ACK)
        return;
    }

    // FAILED or FLASH_NOT_IMPLEMENTED (legacy) — record immediately and advance.
    results_.push_back({currentControllerId_, mapped.padawanStatus, "", mapped.errorReason});
    finishCurrentPadawanAndAdvance();
}

void OtaForwarder::handleFlashResultTimeout()
{
    if (phase_ != Phase::AWAITING_FLASH_RESULT)
    {
        return; // stale fire after we already completed
    }
    ESP_LOGW(TAG, "Flash-result timeout for %s; recording FAILED", currentControllerId_.c_str());
    results_.push_back({currentControllerId_, PadawanStatus::FAILED, "", "flash_result_timeout"});
    finishCurrentPadawanAndAdvance();
}

bool OtaForwarder::versionConfirmTimerStart()
{
    if (versionConfirmTimer_ == nullptr)
    {
        ESP_LOGE(TAG, "versionConfirmTimerStart: timer handle is null");
        return false;
    }
    esp_timer_stop(versionConfirmTimer_);
    esp_err_t err = esp_timer_start_once(versionConfirmTimer_, 15ULL * 1000ULL * 1000ULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "versionConfirmTimerStart: esp_timer_start_once failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

void OtaForwarder::versionConfirmTimerStop()
{
    if (versionConfirmTimer_ == nullptr)
    {
        return;
    }
    esp_timer_stop(versionConfirmTimer_);
}

void OtaForwarder::handleVersionConfirmTimeout()
{
    if (phase_ != Phase::AWAITING_VERSION_CONFIRMED)
    {
        // Stale fire after we already completed via tick-driven match.
        return;
    }
    ESP_LOGW(TAG, "Version-confirm timeout for %s; recording FAILED(version_unconfirmed)",
             currentControllerId_.c_str());
    results_.push_back({currentControllerId_, PadawanStatus::FAILED, "", "version_unconfirmed"});
    finishCurrentPadawanAndAdvance();
}

void OtaForwarder::checkPeerVersionForCurrentPadawan()
{
    if (phase_ != Phase::AWAITING_VERSION_CONFIRMED)
    {
        return;
    }
    if (expectedNewVersion_.empty())
    {
        // Parse failed at deploy start — no comparison possible. The 15 s
        // versionConfirmTimer will fire and record FAILED("version_unconfirmed").
        return;
    }

    std::string reported = AstrOs_EspNow.getPeerVersion(currentControllerId_);
    if (reported.empty() || reported != expectedNewVersion_)
    {
        // Still on old version (or no version reported yet); keep waiting
        // until either match or timeout.
        return;
    }

    ESP_LOGI(TAG, "Version confirmed for %s: '%s' == expected '%s'", currentControllerId_.c_str(), reported.c_str(),
             expectedNewVersion_.c_str());

    versionConfirmTimerStop();

    AstrOs_SerialMsgHandler.sendFwProgress(deployTransferId_, currentControllerId_, "VERSION_CONFIRMED",
                                           firmwareTotalSize_, firmwareTotalSize_, expectedNewVersion_);

    results_.push_back({currentControllerId_, PadawanStatus::OK, expectedNewVersion_, ""});
    finishCurrentPadawanAndAdvance();
}

bool OtaForwarder::resolveControllerMac(const std::string &controllerId, uint8_t outMac[6]) const
{
    // Linear scan over AstrOs_EspNow.getPeers() — list is bounded by
    // ESPNOW_PEER_LIMIT (10), so the cost is negligible. getPeers
    // internally acquires the peer mutex and returns a vector copy, so
    // the read is thread-safe.
    //
    // controllerId is a MAC string ("XX:XX:XX:XX:XX:XX", uppercase), as
    // populated by the Pi-side controllerVariantCache and forwarded in
    // FW_DEPLOY_BEGIN. MAC is the canonical identity on the ESP side.
    auto peers = AstrOs_EspNow.getPeers();
    for (const auto &p : peers)
    {
        if (controllerId == AstrOsStringUtils::macToString(p.mac_addr))
        {
            std::memcpy(outMac, p.mac_addr, 6);
            return true;
        }
    }
    return false;
}

bool OtaForwarder::isFromCurrentPadawan(const uint8_t srcMac[6]) const
{
    return std::memcmp(srcMac, currentPadawanMac_, 6) == 0;
}

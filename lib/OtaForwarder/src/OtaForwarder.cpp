#include "OtaForwarder.hpp"

#include <AstrOsEspNow.h>
#include <AstrOsMessaging.hpp>
#include <AstrOsSerialMsgHandler.hpp>
#include <AstrOsSha256.h>
#include <OtaReceiver.hpp>
#include <esp_log.h>

#include <algorithm>
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

// Task 7b fills in the data/end-side bodies. Begin-side flow is implemented below.
void OtaForwarder::handleDeployBegin(queue_ota_forwarder_msg_t &msg)
{
    if (phase_ != Phase::IDLE)
    {
        ESP_LOGW(TAG, "handleDeployBegin while not IDLE (phase=%d); rejecting", (int)phase_);
        // Reply all-FAILED so the JobLock releases on the server side.
        std::vector<astros_fw_deploy_result_t> failures;
        // Don't have an order list parsed yet; best we can do is one
        // synthetic FAILED so the server sees something terminal.
        failures.push_back({"unknown", "FAILED", "", "forwarder_busy"});
        std::string msgId = msg.deploy.msgId ? msg.deploy.msgId : "";
        std::string xferId = msg.transferId ? msg.transferId : "";
        AstrOs_SerialMsgHandler.sendFwDeployDone(msgId, xferId, failures);
        return;
    }

    deployMsgId_ = msg.deploy.msgId ? msg.deploy.msgId : "";
    deployTransferId_ = msg.transferId ? msg.transferId : "";

    // Parse the RS-separated order list into a vector.
    orderList_.clear();
    if (msg.deploy.orderList != nullptr)
    {
        std::string raw = msg.deploy.orderList;
        size_t start = 0;
        while (start < raw.size())
        {
            size_t end = raw.find('\x1E', start);
            std::string id = (end == std::string::npos) ? raw.substr(start) : raw.substr(start, end - start);
            if (!id.empty())
            {
                orderList_.push_back(id);
            }
            if (end == std::string::npos)
            {
                break;
            }
            start = end + 1;
        }
    }

    if (orderList_.empty())
    {
        ESP_LOGW(TAG, "FW_DEPLOY_BEGIN orderList empty — dropping");
        std::vector<astros_fw_deploy_result_t> empty;
        AstrOs_SerialMsgHandler.sendFwDeployDone(deployMsgId_, deployTransferId_, empty);
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
    // Streaming drains will happen on the next tick (Task 7b's handleTick
    // body covers this). For now, kick once to start the flow.
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
    ESP_LOGW(TAG, "handleDataAck: stubbed (Task 7b)");
    (void)msg;
}
void OtaForwarder::handleDataNak(queue_ota_forwarder_msg_t &msg)
{
    ESP_LOGW(TAG, "handleDataNak: stubbed (Task 7b)");
    (void)msg;
}
void OtaForwarder::handleEndAck(queue_ota_forwarder_msg_t &msg)
{
    ESP_LOGW(TAG, "handleEndAck: stubbed (Task 7b)");
    (void)msg;
}
void OtaForwarder::handleTick()
{
    // Stub for Task 7b; deliberately silent on the hot path.
}

void OtaForwarder::startNextPadawan()
{
    // Skip the "master" entry (PR set 2 will self-flash); record as FAILED.
    while (nextOrderIdx_ < orderList_.size() && orderList_[nextOrderIdx_] == "master")
    {
        results_.push_back({orderList_[nextOrderIdx_], "FAILED", "", "not_implemented_in_pr_set_1"});
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
        results_.push_back({currentControllerId_, "FAILED", "", "unknown_peer"});
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
            results_.push_back({orderList_[nextOrderIdx_], "FAILED", "", "no_firmware"});
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
        results_.push_back({currentControllerId_, "FAILED", "", "firmware_open_failed"});
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
        results_.push_back({currentControllerId_, "FAILED", "", "firmware_stat_failed"});
        nextOrderIdx_++;
        startNextPadawan();
        return;
    }
    firmwareTotalSize_ = static_cast<uint32_t>(st.st_size);
    firmwareTotalChunks_ = (firmwareTotalSize_ + kChunkSize - 1) / kChunkSize;

    // Compute SHA-256 of the file (forensic-grade defensive check; the
    // padawan also verifies). One-shot at file open; ships in the
    // OTA_BEGIN frame's sha256Expected field.
    AstrOsSha256Ctx shaCtx;
    AstrOsSha256_init(&shaCtx);
    uint8_t buf[1024];
    while (size_t r = std::fread(buf, 1, sizeof(buf), firmwareFile_))
    {
        AstrOsSha256_update(&shaCtx, buf, r);
    }
    AstrOsSha256_final(&shaCtx, firmwareSha256_);
    std::rewind(firmwareFile_);

    currentXferId_ = static_cast<uint8_t>(nextOrderIdx_ + 1); // monotonic per-padawan xferId

    auto br = bulk_.begin(currentXferId_, firmwareTotalChunks_, kChunkSize, kWindowSize, kAckTimeoutMs, kMaxRetries);
    if (!br.valid)
    {
        ESP_LOGE(TAG, "BulkSender::begin rejected reason=%d", (int)br.reason);
        std::fclose(firmwareFile_);
        firmwareFile_ = nullptr;
        results_.push_back({currentControllerId_, "FAILED", "", "begin_rejected"});
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
    results_.push_back({currentControllerId_, "FAILED", "", reason});
    currentControllerId_.clear();
    phase_ = Phase::BETWEEN_PADAWANS;
    nextOrderIdx_++;
    startNextPadawan();
}
void OtaForwarder::completeCurrentPadawan()
{
    ESP_LOGW(TAG, "completeCurrentPadawan: stubbed (Task 7b)");
}
void OtaForwarder::emitDeployDoneAndReset()
{
    ESP_LOGI(TAG, "FW_DEPLOY_DONE: %zu targets, transferId=%s", results_.size(), deployTransferId_.c_str());
    // Convert OtaForwarderPadawanResult rows to astros_fw_deploy_result_t.
    // The two types mirror each other field-for-field; the conversion is
    // mechanical, kept here so the forwarder's internal type can evolve
    // independently of the serial-message struct.
    std::vector<astros_fw_deploy_result_t> wire;
    wire.reserve(results_.size());
    for (const auto &r : results_)
    {
        wire.push_back({r.controllerId, r.status, r.finalVersion, r.errorOrEmpty});
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
        ESP_LOGW(TAG, "OTA_BEGIN sendOtaFrame returned %s; the BEGIN_ACK timeout will catch this",
                 esp_err_to_name(err));
    }
}
void OtaForwarder::emitOtaEndFrame()
{
    ESP_LOGW(TAG, "emitOtaEndFrame: stubbed (Task 7b)");
}
void OtaForwarder::streamDrain(uint64_t nowMs)
{
    (void)nowMs;
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
    // Post a synthetic NAK-equivalent so all state transitions happen on
    // otaForwarderTask. Sentinel xferId=0xFF + reason=0xFF distinguishes
    // the timeout from a real wire NAK; handleBeginNak detects it and
    // routes to abortCurrentPadawan("begin_ack_timeout").
    queue_ota_forwarder_msg_t m{};
    m.kind = OTA_FWD_BEGIN_NAK;
    m.begin_nak.xferId = 0xFF;
    m.begin_nak.reason = 0xFF;
    xQueueSend(self->otaForwarderQueue_, &m, 0);
}

void OtaForwarder::endAckTimerCb(void *arg)
{
    (void)arg;
    // See beginAckTimerCb note. Task 7b implements.
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

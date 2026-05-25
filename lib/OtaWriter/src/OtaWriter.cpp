#include <OtaWriter.hpp>

#include <AstrOsEspNowService.hpp>
#include <esp_log.h>

#include <cstring>

static const char *TAG = "OtaWriter";

OtaWriter AstrOs_OtaWriter;

OtaWriter::OtaWriter() {}

OtaWriter::~OtaWriter()
{
    // The singleton is process-lifetime in firmware; this dtor exists for
    // native-link symmetry (when/if a future host build links OtaWriter).
    // esp_timer_stop on an idle timer is harmless.
    if (watchdog_ != nullptr)
    {
        esp_timer_stop(watchdog_);
        esp_timer_delete(watchdog_);
        watchdog_ = nullptr;
    }
    resetOtaHandleAndSha();
}

void OtaWriter::Init(QueueHandle_t otaWriterQueue)
{
    // Idempotent: a second Init() would leak the first esp_timer handle.
    if (watchdog_ != nullptr)
    {
        ESP_LOGW(TAG, "Init() called twice — ignoring second call");
        return;
    }

    otaWriterQueue_ = otaWriterQueue;

    const esp_timer_create_args_t args = {
        .callback = &OtaWriter::watchdogTimerCb,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ota_writer_watchdog",
        .skip_unhandled_events = false,
    };
    esp_err_t err = esp_timer_create(&args, &watchdog_);
    if (err != ESP_OK)
    {
        // Writer still serves happy-path transfers without the watchdog —
        // only stuck-recovery is lost. Mirrors OtaReceiver's choice.
        ESP_LOGE(TAG, "esp_timer_create(ota_writer_watchdog) failed: %s — watchdog disabled", esp_err_to_name(err));
        watchdog_ = nullptr;
    }

    ESP_LOGI(TAG, "OtaWriter initialized (watchdog idle threshold: %llums)", kWatchdogIdleUs / 1000ULL);
}

void OtaWriter::watchdogTimerCb(void *arg)
{
    auto self = static_cast<OtaWriter *>(arg);
    if (self->otaWriterQueue_ == nullptr)
    {
        return;
    }
    queue_ota_writer_msg_t msg{};
    msg.kind = OTA_WR_WATCHDOG_FIRE;
    // Non-blocking — a full queue means otaWriterTask is already wedged;
    // dropping the signal is the least-bad outcome.
    if (xQueueSend(self->otaWriterQueue_, &msg, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "watchdog: otaWriterQueue full, dropping WATCHDOG_FIRE signal");
    }
}

void OtaWriter::watchdogStart()
{
    if (watchdog_ == nullptr)
        return;
    esp_err_t err = esp_timer_start_once(watchdog_, kWatchdogIdleUs);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "watchdog: esp_timer_start_once failed: %s", esp_err_to_name(err));
    }
}

void OtaWriter::watchdogRestart()
{
    if (watchdog_ == nullptr)
        return;
    // esp_timer's one-shot has no native restart; stop-then-start.
    // esp_timer_stop on a non-running timer returns ESP_ERR_INVALID_STATE — benign.
    esp_timer_stop(watchdog_);
    esp_err_t err = esp_timer_start_once(watchdog_, kWatchdogIdleUs);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "watchdog: restart esp_timer_start_once failed: %s", esp_err_to_name(err));
    }
}

void OtaWriter::watchdogStop()
{
    if (watchdog_ == nullptr)
        return;
    esp_timer_stop(watchdog_);
}

void OtaWriter::resetOtaHandleAndSha()
{
    if (otaHandle_ != 0)
    {
        // esp_ota_abort releases the handle. Return value isn't actionable
        // here — we're already on the abort path.
        esp_err_t err = esp_ota_abort(otaHandle_);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "esp_ota_abort returned %s — partition may be in inconsistent state", esp_err_to_name(err));
        }
        otaHandle_ = 0;
    }
    // AstrOsSha256 owns no heap; just drop the active flag.
    shaActive_ = false;
    bulk_.reset();
    inactivePartition_ = nullptr;
    currentXferId_ = 0;
    memset(currentMasterMac_, 0, sizeof(currentMasterMac_));
    currentTotalSize_ = 0;
    memset(expectedSha256_, 0, sizeof(expectedSha256_));
    active_ = false;
}

void OtaWriter::process(queue_ota_writer_msg_t &msg)
{
    switch (msg.kind)
    {
    case OTA_WR_BEGIN:
        handleBegin(msg);
        break;
    case OTA_WR_DATA:
        handleData(msg);
        break;
    case OTA_WR_END:
        handleEnd(msg);
        break;
    case OTA_WR_WATCHDOG_FIRE:
        handleWatchdogFire();
        break;
    default:
        ESP_LOGE(TAG, "process: unknown msg.kind=%d", (int)msg.kind);
        break;
    }
    // Convention (mirrors OtaReceiver::process / OtaForwarder::process):
    // process() owns the free contract. Callers (otaWriterTask) must NOT
    // free externally — doing so would double-free.
    freeOtaWriterMsg(&msg);
}

// ─── Stubbed handlers — Tasks 5/6/7/8 fill in the bodies ────────────────

void OtaWriter::handleBegin(queue_ota_writer_msg_t &msg)
{
    const uint8_t *mac = msg.begin.srcMac;
    const uint8_t xferId = msg.begin.xferId;

    if (active_)
    {
        ESP_LOGW(TAG, "handleBegin: xferId=%u arrived while xferId=%u is active — replying BUSY", xferId,
                 currentXferId_);
        sendBeginNak(mac, xferId, OtaBeginNakReason::BUSY);
        return;
    }

    // BulkReceiver wants a uint8_t xferId; rec.xferId is already a uint8_t
    // from the wire, no parse needed.

    // Inactive partition lookup. esp_ota_get_next_update_partition(NULL)
    // returns the slot that's NOT currently the boot partition. On a fresh
    // factory image where ota_data hasn't been written yet, ESP-IDF picks
    // ota_0; once we've ever booted from ota_0, this returns ota_1.
    inactivePartition_ = esp_ota_get_next_update_partition(NULL);
    if (inactivePartition_ == nullptr)
    {
        ESP_LOGE(
            TAG,
            "handleBegin: esp_ota_get_next_update_partition returned NULL — partition table is missing ota_0/ota_1");
        sendBeginNak(mac, xferId, OtaBeginNakReason::NO_PARTITION);
        return;
    }

    if (msg.begin.totalSize > inactivePartition_->size)
    {
        ESP_LOGW(TAG, "handleBegin: totalSize=%u exceeds partition '%s' size=%u — NAK NO_PARTITION",
                 (unsigned)msg.begin.totalSize, inactivePartition_->label, (unsigned)inactivePartition_->size);
        inactivePartition_ = nullptr;
        sendBeginNak(mac, xferId, OtaBeginNakReason::NO_PARTITION);
        return;
    }

    // esp_ota_begin allocates internal state + reserves the partition for
    // writing. OTA_SIZE_UNKNOWN tells it to erase only the sectors we
    // actually write (lazy per-sector erase inside esp_ota_write). Passing
    // the exact totalSize would let ESP-IDF erase the whole reserved span
    // up front — that's a one-shot erase of ~2 MB (8MB board) or ~6.4 MB
    // (16MB board), which can exceed the BEGIN_ACK timeout window. Lazy
    // per-sector erase keeps the M4 happy-path latency budget intact;
    // bench data via the M4 checkpoint validates this assumption
    // (resolves design open-Q #5).
    esp_err_t bErr = esp_ota_begin(inactivePartition_, OTA_SIZE_UNKNOWN, &otaHandle_);
    if (bErr != ESP_OK)
    {
        ESP_LOGE(TAG, "handleBegin: esp_ota_begin failed: %s — NAK BEGIN_FAILED", esp_err_to_name(bErr));
        otaHandle_ = 0;
        inactivePartition_ = nullptr;
        sendBeginNak(mac, xferId, OtaBeginNakReason::BEGIN_FAILED);
        return;
    }

    // BulkReceiver windowSize matches the master's BulkSender window (8) —
    // see lib/OtaForwarder/include/OtaForwarder.hpp:156's kWindowSize.
    constexpr uint8_t kWindowSize = 8;
    auto br = bulk_.begin(xferId, msg.begin.totalSize, msg.begin.totalChunks, msg.begin.chunkSize, kWindowSize);
    if (!br.valid)
    {
        ESP_LOGW(TAG, "handleBegin: BulkReceiver::begin rejected: reason=%d (totalSize=%u chunks=%u chunkSize=%u)",
                 (int)br.reason, (unsigned)msg.begin.totalSize, (unsigned)msg.begin.totalChunks,
                 (unsigned)msg.begin.chunkSize);
        // esp_ota_begin succeeded but BulkReceiver setup failed —
        // resetOtaHandleAndSha clears the partial state.
        resetOtaHandleAndSha();
        sendBeginNak(mac, xferId, OtaBeginNakReason::BEGIN_FAILED);
        return;
    }

    AstrOsSha256_init(&shaCtx_);
    shaActive_ = true;

    // Latch per-transfer state.
    currentXferId_ = xferId;
    memcpy(currentMasterMac_, mac, 6);
    currentTotalSize_ = msg.begin.totalSize;
    memcpy(expectedSha256_, msg.begin.sha256Expected, 32);
    active_ = true;

    ESP_LOGI(
        TAG,
        "handleBegin accepted: xferId=%u totalSize=%u chunks=%u chunkSize=%u partition='%s' (size=%u, offset=0x%lx)",
        xferId, (unsigned)msg.begin.totalSize, (unsigned)msg.begin.totalChunks, (unsigned)msg.begin.chunkSize,
        inactivePartition_->label, (unsigned)inactivePartition_->size, (unsigned long)inactivePartition_->address);

    esp_err_t sendErr = sendBeginAck(mac, xferId);
    if (sendErr != ESP_OK)
    {
        ESP_LOGW(TAG, "handleBegin: sendBeginAck failed: %s — relying on master tick-retransmit",
                 esp_err_to_name(sendErr));
        // Don't abort the transfer on ACK send failure — master's
        // BEGIN_ACK timeout (2 s) will fire if the ACK never lands, at
        // which point master retransmits OTA_BEGIN. Padawan-side state
        // stays committed; the second BEGIN will hit the BUSY path above
        // and return another ACK attempt.
    }
    watchdogStart();
}

void OtaWriter::handleData(queue_ota_writer_msg_t &msg)
{
    ESP_LOGW(TAG, "handleData: stubbed (Task 6) — xferId=%u seq=%u", msg.data.xferId, msg.data.seq);
}

void OtaWriter::handleEnd(queue_ota_writer_msg_t &msg)
{
    ESP_LOGW(TAG, "handleEnd: stubbed (Task 7) — xferId=%u chunks=%u", msg.end.xferId, msg.end.totalChunksSent);
}

void OtaWriter::handleWatchdogFire()
{
    ESP_LOGW(TAG, "handleWatchdogFire: stubbed (Task 8)");
}

// ─── Wire-emission helpers — Tasks 5/6/7 use these ──────────────────────

esp_err_t OtaWriter::sendBeginAck(const uint8_t mac[6], uint8_t xferId)
{
    OtaBeginAckPayload p{};
    p.xferId = xferId;
    return AstrOs_EspNow.sendOtaFrame(mac, AstrOsPacketType::OTA_BEGIN_ACK, reinterpret_cast<const uint8_t *>(&p),
                                      sizeof(p));
}

esp_err_t OtaWriter::sendBeginNak(const uint8_t mac[6], uint8_t xferId, OtaBeginNakReason reason)
{
    OtaBeginNakPayload p{};
    p.xferId = xferId;
    p.reason = static_cast<uint8_t>(reason);
    return AstrOs_EspNow.sendOtaFrame(mac, AstrOsPacketType::OTA_BEGIN_NAK, reinterpret_cast<const uint8_t *>(&p),
                                      sizeof(p));
}

esp_err_t OtaWriter::sendDataAck(const uint8_t mac[6], uint8_t xferId, uint32_t highestContiguousSeq,
                                 uint32_t nextExpectedSeq, uint8_t windowRemaining)
{
    OtaDataAckPayload p{};
    p.xferId = xferId;
    p.highestContiguousSeq = highestContiguousSeq;
    p.nextExpectedSeq = nextExpectedSeq;
    p.windowRemaining = windowRemaining;
    return AstrOs_EspNow.sendOtaFrame(mac, AstrOsPacketType::OTA_DATA_ACK, reinterpret_cast<const uint8_t *>(&p),
                                      sizeof(p));
}

esp_err_t OtaWriter::sendDataNak(const uint8_t mac[6], uint8_t xferId, uint32_t highestContiguousSeq,
                                 uint32_t nextExpectedSeq, uint8_t windowRemaining, OtaDataNakReason reason)
{
    OtaDataNakPayload p{};
    p.xferId = xferId;
    p.highestContiguousSeq = highestContiguousSeq;
    p.nextExpectedSeq = nextExpectedSeq;
    p.windowRemaining = windowRemaining;
    p.reason = static_cast<uint8_t>(reason);
    return AstrOs_EspNow.sendOtaFrame(mac, AstrOsPacketType::OTA_DATA_NAK, reinterpret_cast<const uint8_t *>(&p),
                                      sizeof(p));
}

esp_err_t OtaWriter::sendEndAck(const uint8_t mac[6], uint8_t xferId, OtaEndStatus status,
                                const uint8_t sha256Computed[32])
{
    OtaEndAckPayload p{};
    p.xferId = xferId;
    p.status = static_cast<uint8_t>(status);
    memcpy(p.sha256Computed, sha256Computed, 32);
    return AstrOs_EspNow.sendOtaFrame(mac, AstrOsPacketType::OTA_END_ACK, reinterpret_cast<const uint8_t *>(&p),
                                      sizeof(p));
}

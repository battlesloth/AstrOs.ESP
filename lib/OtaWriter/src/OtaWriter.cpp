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
}

// ─── Stubbed handlers — Tasks 5/6/7/8 fill in the bodies ────────────────

void OtaWriter::handleBegin(queue_ota_writer_msg_t &msg)
{
    ESP_LOGW(TAG, "handleBegin: stubbed (Task 5) — xferId=%u totalSize=%u", msg.begin.xferId, msg.begin.totalSize);
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

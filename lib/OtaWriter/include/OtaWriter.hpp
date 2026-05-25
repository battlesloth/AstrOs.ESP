#ifndef OTAWRITER_HPP
#define OTAWRITER_HPP

#include <AstrOsBulkTransport.hpp>
#include <AstrOsSha256.h>
#include <OtaWriterQueueMessage.h>

#include <atomic>
#include <cstdint>

// needed for QueueHandle_t, must be in this order
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_timer.h>

// Threading: all members are accessed only from otaWriterTask via
// `process(msg)`. The one exception is `active_` (atomic; read from the
// pollingTimer's esp_timer dispatch task for polling-pause gating).
//
// The watchdog timer fires from esp_timer's dispatch task, but its
// callback only does xQueueSend(OTA_WR_WATCHDOG_FIRE) — state mutation
// runs on otaWriterTask, preserving the single-task-state invariant.

class OtaWriter
{
public:
    OtaWriter();
    ~OtaWriter();

    OtaWriter(const OtaWriter &) = delete;
    OtaWriter &operator=(const OtaWriter &) = delete;
    OtaWriter(OtaWriter &&) = delete;
    OtaWriter &operator=(OtaWriter &&) = delete;

    // Two-step construction (mirror OtaReceiver / OtaForwarder): the
    // singleton can be built at static-init time, FreeRTOS queues attach
    // later. The queue handle is held so the watchdog callback can post
    // into the same queue otaWriterTask drains.
    void Init(QueueHandle_t otaWriterQueue);

    // Single entry point from otaWriterTask.
    void process(queue_ota_writer_msg_t &msg);

    // Safe to call from any task. Gates polling work during a transfer.
    bool isActive() const noexcept
    {
        return active_;
    }

private:
    // Per-handler entry points. All run on otaWriterTask.
    void handleBegin(queue_ota_writer_msg_t &msg);
    void handleData(queue_ota_writer_msg_t &msg);
    void handleEnd(queue_ota_writer_msg_t &msg);
    void handleWatchdogFire();

    // Idempotent cleanup. Calls esp_ota_abort if otaHandle_ != 0, drops
    // shaActive_, resets BulkReceiver, clears partition/size fields,
    // sets active_ = false. Invoked from every abort path (data-write
    // fail, end-side failure, watchdog fire, dtor). Mirrors
    // OtaReceiver::resetCryptoAndFile's invariant: every exit path routes
    // through this so we never leak an OTA handle across transfers.
    void resetOtaHandleAndSha();

    // esp_timer's one-shot has no native "restart" — these centralize the
    // stop-then-start pattern.
    void watchdogStart();
    void watchdogRestart();
    void watchdogStop();

    // esp_timer callback indirection — arg is `this`. Callback ONLY posts
    // OTA_WR_WATCHDOG_FIRE; state mutation happens on otaWriterTask.
    static void watchdogTimerCb(void *arg);

    // Emits an OTA_BEGIN_ACK / NAK frame via AstrOs_EspNow.sendOtaFrame.
    // Returns esp_err_t from the underlying send. Caller logs but does not
    // act on the result — a failed reply will be re-elicited by the
    // master's tick-driven retransmit.
    esp_err_t sendBeginAck(const uint8_t mac[6], uint8_t xferId);
    esp_err_t sendBeginNak(const uint8_t mac[6], uint8_t xferId, OtaBeginNakReason reason);
    esp_err_t sendDataAck(const uint8_t mac[6], uint8_t xferId, uint32_t highestContiguousSeq, uint32_t nextExpectedSeq,
                          uint8_t windowRemaining);
    esp_err_t sendDataNak(const uint8_t mac[6], uint8_t xferId, uint32_t highestContiguousSeq, uint32_t nextExpectedSeq,
                          uint8_t windowRemaining, OtaDataNakReason reason);
    esp_err_t sendEndAck(const uint8_t mac[6], uint8_t xferId, OtaEndStatus status, const uint8_t sha256Computed[32]);

    // Logs at WARN if a wire-frame send failed. Replies are advisory only —
    // master will retry/abandon on its own timeout if the reply doesn't land.
    // Centralizes the per-NAK-site send-failure log so 14 call sites don't
    // each carry an inline if-block.
    void logSendResult(const char *site, esp_err_t err);

    std::atomic<bool> active_{false};
    QueueHandle_t otaWriterQueue_ = nullptr;

    // 10 s idle threshold — mirrors OtaReceiver.
    esp_timer_handle_t watchdog_ = nullptr;
    static constexpr uint64_t kWatchdogIdleUs = 10ULL * 1000ULL * 1000ULL;

    // BulkReceiver (existing PURE lib) handles seq tracking + windowing.
    AstrOsBulkTransport::BulkReceiver bulk_;

    // Per-transfer state — live between handleBegin success and the
    // terminating handleEnd / abort. resetOtaHandleAndSha() restores
    // all-zero / nullptr.
    const esp_partition_t *inactivePartition_ = nullptr;
    esp_ota_handle_t otaHandle_ = 0; // 0 means "no handle open"
    AstrOsSha256Ctx shaCtx_;
    bool shaActive_ = false; // gates init-vs-update ordering without explicit free
    uint8_t currentXferId_ = 0;
    uint8_t currentMasterMac_[6] = {0};
    uint32_t currentTotalSize_ = 0;
    uint8_t expectedSha256_[32] = {0};
};

extern OtaWriter AstrOs_OtaWriter;

#endif

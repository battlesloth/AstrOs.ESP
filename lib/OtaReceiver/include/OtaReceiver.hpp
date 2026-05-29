#ifndef OTARECEIVER_HPP
#define OTARECEIVER_HPP

#include <AstrOsBulkTransport.hpp>
#include <AstrOsSha256.h>
#include <OtaQueueMessage.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>

// needed for QueueHandle_t, must be in this order
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <esp_timer.h>

// Threading: all members are accessed only from otaReceiverTask via
// `process(...)`. The one exception is `active_` (see its declaration), which
// is read from the polling timer's dispatch task. Reply emission via
// AstrOs_SerialMsgHandler.sendFw*() is independently thread-safe.
//
// The idle watchdog timer fires from esp_timer's dispatch task, but its
// callback only does xQueueSend(OTA_MSG_WATCHDOG_FIRE) — state mutation still
// runs on otaReceiverTask, preserving the single-task-state invariant.
class OtaReceiver
{
private:
    AstrOsBulkTransport::BulkReceiver bulk_;
    // `active_` is atomic because `isActive()` is called from the
    // pollingTimer callback (esp_timer dispatch task) to gate poll work
    // during OTA — see src/main.cpp's pollingTimerCallback. Writes still
    // only happen from otaReceiverTask via handleBegin/End/WatchdogFire.
    std::atomic<bool> active_{false};
    // Identifies the in-flight transfer in the busy-reject log (handleBegin)
    // and the watchdog-fire log (handleWatchdogFire). NOT echoed in ACK/NAK
    // replies — those use the inbound message's own transferId.
    std::string transferIdStr_;

    // Idle-activity watchdog. Fires if no chunk activity within kWatchdogIdleUs
    // (transfer stuck after BEGIN — server crash, link drop, all-NAKs). Callback
    // posts OTA_MSG_WATCHDOG_FIRE for in-task abort.
    esp_timer_handle_t watchdog_ = nullptr;
    QueueHandle_t otaQueue_ = nullptr;
    static constexpr uint64_t kWatchdogIdleUs = 10ULL * 1000ULL * 1000ULL; // 10 s

    // Staging-file handle and streaming SHA-256 context. Both are live only
    // between a successful BEGIN and the terminating END / abort. Every exit
    // path routes through resetCryptoAndFile() so we never leak an open FILE*
    // across transfers. AstrOsSha256 is pure software (no shared hardware
    // engine), avoiding the cross-task race we observed when mbedtls' OTA
    // context's state got clobbered by another SHA consumer; `shaActive_`
    // gates init-vs-update ordering without needing an explicit free.
    FILE *staging_ = nullptr;
    AstrOsSha256Ctx shaCtx_;
    bool shaActive_ = false;

    // Set inside handleEnd's success-rename branch. Read by
    // getLastFirmwarePath() under lastFirmwareMutex_.
    mutable std::mutex lastFirmwareMutex_;
    std::string lastFirmwarePath_;

public:
    OtaReceiver();
    ~OtaReceiver();

    // Singleton; copy/move would silently create a second receiver pointing at
    // the same queue and timer.
    OtaReceiver(const OtaReceiver &) = delete;
    OtaReceiver &operator=(const OtaReceiver &) = delete;
    OtaReceiver(OtaReceiver &&) = delete;
    OtaReceiver &operator=(OtaReceiver &&) = delete;

    // Split from the constructor so the global can be constructed at static-init
    // time, before FreeRTOS queues exist. The queue handle is held so the
    // watchdog callback can post into the same queue otaReceiverTask drains.
    void Init(QueueHandle_t otaQueue);

    void process(queue_ota_msg_t &msg);

    // Safe to call from any task. Gates poll work during OTA.
    bool isActive() const noexcept
    {
        return active_;
    }

    // Returns the path of the most recently successfully-renamed firmware,
    // or std::nullopt if no firmware has been received yet. Set by
    // handleEnd's success-rename branch. Thread-safe; OtaForwarder
    // (different task on same core) reads this on FW_DEPLOY_BEGIN to
    // locate the staged .bin file.
    //
    // Single-firmware-at-a-time assumption: the typical server flow is
    // "upload, then immediately deploy". Multi-firmware tracking (by
    // transferId or sha) is a future-milestone concern.
    std::optional<std::string> getLastFirmwarePath() const;

private:
    void handleBegin(queue_ota_msg_t &msg);
    void handleChunk(queue_ota_msg_t &msg);
    void handleEnd(queue_ota_msg_t &msg);
    void handleWatchdogFire();

    // esp_timer's one-shot has no native "restart" — these centralize the
    // stop-then-start pattern.
    void watchdogStart();
    void watchdogRestart();
    void watchdogStop();

    // esp_timer callback indirection — arg is `this`.
    static void watchdogTimerCb(void *arg);

    // Closes staging_ (unlinking unless keepStaging) and releases shaCtx_.
    // Idempotent so callers don't need to track which fields are live.
    void resetCryptoAndFile(bool keepStaging);
};

extern OtaReceiver AstrOs_OtaReceiver;

#endif

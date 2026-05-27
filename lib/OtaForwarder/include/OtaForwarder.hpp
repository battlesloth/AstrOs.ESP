#ifndef OTAFORWARDER_HPP
#define OTAFORWARDER_HPP

#include <AstrOsBulkTransport.hpp>
#include <OtaForwarderQueueMessage.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <esp_timer.h>

// Threading: all members are accessed only from otaForwarderTask via
// `process(msg)`. The one exception is `active_` (atomic; read from the
// pollingTimer's esp_timer dispatch task for polling-pause gating).
//
// The tick timer fires from esp_timer's dispatch task, but its callback
// only does xQueueSend(OTA_FWD_TICK) — state mutation runs on
// otaForwarderTask, preserving the single-task-state invariant.

class OtaForwarder
{
public:
    OtaForwarder();
    ~OtaForwarder();

    OtaForwarder(const OtaForwarder &) = delete;
    OtaForwarder &operator=(const OtaForwarder &) = delete;
    OtaForwarder(OtaForwarder &&) = delete;
    OtaForwarder &operator=(OtaForwarder &&) = delete;

    // Two-step construction (mirror OtaReceiver): the singleton can be
    // built at static-init time, FreeRTOS queues attach later.
    void Init(QueueHandle_t otaForwarderQueue);

    // Single entry point from otaForwarderTask.
    void process(queue_ota_forwarder_msg_t &msg);

    // Safe to call from any task. Gates polling work during a forward.
    bool isActive() const noexcept
    {
        return active_;
    }

private:
    // State machine (single in-flight transfer; sequential per-padawan).
    enum class Phase : uint8_t
    {
        IDLE = 0,
        AWAITING_BEGIN_ACK = 1, // emitted OTA_BEGIN, waiting on padawan
        STREAMING = 2,
        AWAITING_END_ACK = 3,
        AWAITING_FLASH_RESULT = 4, // END_ACK OK received; padawan verifying + committing
        BETWEEN_PADAWANS = 5,      // result recorded; pulling next from order list
        // Note: there is no DONE state — emitDeployDoneAndReset transitions
        // straight back to IDLE after emitting FW_DEPLOY_DONE.
    };

    // Stringified to OK/FAILED at the wire boundary. If you add a value
    // here, update emitDeployDoneAndReset's switch.
    enum class PadawanStatus : uint8_t
    {
        OK,
        FAILED,
    };

    struct PadawanResult
    {
        std::string controllerId;
        PadawanStatus status;
        std::string finalVersion; // empty until version-confirmation lands
        std::string errorOrEmpty; // failure reason ("begin_ack_timeout", etc.)
    };

    // Per-handler entry points. All run on otaForwarderTask.
    void handleDeployBegin(queue_ota_forwarder_msg_t &msg);
    void handleBeginAck(queue_ota_forwarder_msg_t &msg);
    void handleBeginNak(queue_ota_forwarder_msg_t &msg);
    void handleDataAck(queue_ota_forwarder_msg_t &msg);
    void handleDataNak(queue_ota_forwarder_msg_t &msg);
    void handleEndAck(queue_ota_forwarder_msg_t &msg);
    void handleTick();

    // Per-padawan lifecycle helpers.
    void startNextPadawan();                             // open file, BulkSender.begin, emit OTA_BEGIN
    void abortCurrentPadawan(const std::string &reason); // record FAILED, advance
    void finishCurrentPadawanAndAdvance(); // shared cleanup: stop all timers, close file, reset bulk, advance index
    void emitDeployDoneAndReset();         // FW_DEPLOY_DONE, return to IDLE

    // Wire-emission helpers.
    void emitOtaBeginFrame();
    void emitOtaEndFrame();
    void streamDrain(uint64_t nowMs); // for each SEND from BulkSender::nextChunkToSend, read+emit OTA_DATA

    // Posts a synthetic timeout sentinel (xferId=0xFF, reason/status=0xFF)
    // of the given kind into otaForwarderQueue_. Used by the timer
    // callbacks (deadline fired) and the emit-frame helpers (fail-fast on
    // send failure). `site` tags the queue-full ESP_LOGE so bench logs
    // identify which caller dropped its sentinel. Safe on a null queue
    // handle (logs + returns).
    void postTimeoutSentinel(ota_forwarder_msg_kind_t kind, const char *site);

    // Tick timer (50 ms cadence — fast enough to keep latency under the
    // 400 ms ack timeout while keeping CPU overhead negligible). Started
    // when a transfer begins, stopped when DONE/abandoned.
    void tickTimerStart();
    void tickTimerStop();
    static void tickTimerCb(void *arg);

    // BEGIN_ACK / END_ACK deadline timers — separate from tickTimer to
    // avoid polluting tick cadence with longer-lived deadlines.
    void beginAckTimerStart();
    void beginAckTimerStop();
    void endAckTimerStart();
    void endAckTimerStop();
    static void beginAckTimerCb(void *arg);
    static void endAckTimerCb(void *arg);

    // Stats periodic timer (2 s cadence) — fired while a transfer is live so
    // bench logs get a low-noise progress heartbeat. Started in
    // startNextPadawan after the first OTA_BEGIN is sent; stopped in
    // finishCurrentPadawanAndAdvance (used by handleFlashResult,
    // handleFlashResultTimeout, abortCurrentPadawan).
    // First emission ~2 s after start, which naturally announces the session
    // is running.
    void statsTimerStart();
    void statsTimerStop();
    static void statsTimerCb(void *arg);
    void handleStatsFire();

    // Flash-result safety timer (10 s one-shot). Armed when END_ACK OK arrives;
    // disarmed when OTA_FLASH_RESULT lands. Fire posts OTA_FWD_FLASH_RESULT_TIMEOUT
    // so the deploy can't block forever on a padawan crash during the pre-flash delay.
    void flashResultTimerStart();
    void flashResultTimerStop();
    static void flashResultTimerCb(void *arg);

    void handleFlashResult(queue_ota_forwarder_msg_t &msg);
    void handleFlashResultTimeout();

    // Resolves a controller-id (from FW_DEPLOY_BEGIN's order list) to a
    // MAC. Linear-scans AstrOs_EspNow.getPeers() — small list, cheap.
    // Returns true if found; fills outMac. Returns false if no peer
    // with that name is registered.
    bool resolveControllerMac(const std::string &controllerId, uint8_t outMac[6]) const;

    // True iff srcMac matches the padawan we're currently transferring to.
    // Guards against stale or misrouted ACK/NAK frames from other peers
    // advancing the BulkSender state. Sentinel-bytes paths (timer cbs)
    // run BEFORE this check so the all-zero srcMac never reaches here.
    bool isFromCurrentPadawan(const uint8_t srcMac[6]) const;

    // Active gate (read by pollingTimer's task).
    std::atomic<bool> active_{false};

    // Queue handle held so timer callbacks can post tick + deadline events
    // into the same queue otaForwarderTask drains.
    QueueHandle_t otaForwarderQueue_ = nullptr;

    // Tick timer (50 ms periodic). esp_timer_handle_t is opaque; held as a
    // bare pointer per ESP-IDF conventions.
    esp_timer_handle_t tickTimer_ = nullptr;
    esp_timer_handle_t beginAckTimer_ = nullptr;
    esp_timer_handle_t endAckTimer_ = nullptr;
    esp_timer_handle_t statsTimer_ = nullptr;
    esp_timer_handle_t flashResultTimer_ = nullptr;

    // Cadence: 50 ms tick, 5 s BEGIN_ACK timeout, 5 s END_ACK timeout,
    // 2 s stats emission, 10 s flash-result safety bound. BEGIN_ACK was 2 s
    // but bench measurements showed padawan BEGIN_ACK round-trip lands around
    // 2.5 s (esp_ota_begin + BulkReceiver::begin + ESP-NOW send back),
    // causing spurious abandonments on healthy peers.
    static constexpr uint64_t kTickPeriodUs = 50ULL * 1000ULL;
    static constexpr uint64_t kBeginAckTimeoutUs = 5ULL * 1000ULL * 1000ULL;
    static constexpr uint64_t kEndAckTimeoutUs = 5ULL * 1000ULL * 1000ULL;
    static constexpr uint64_t kStatsPeriodUs = 2ULL * 1000ULL * 1000ULL;
    static constexpr uint64_t kFlashResultTimeoutUs = 10ULL * 1000ULL * 1000ULL;

    // BulkSender params (from the frozen contract).
    static constexpr uint16_t kChunkSize = 128;
    static constexpr uint8_t kWindowSize = 8;
    static constexpr uint32_t kAckTimeoutMs = 400;
    static constexpr uint8_t kMaxRetries = 3;

    // Hard upper bound on the order list size. Must stay well below 254 so
    // that currentXferId_ = nextOrderIdx_ + 1 never produces 0 (no-xfer
    // sentinel) or 0xFF (timeout sentinel). 32 is generous beyond the
    // current ESPNOW_PEER_LIMIT=10 with plenty of margin for future growth.
    static constexpr size_t kMaxOrderListSize = 32;
    static_assert(kMaxOrderListSize < 0xFE, "xferId derivation would wrap into sentinel range");

    // Per-deploy state.
    AstrOsBulkTransport::BulkSender bulk_;
    Phase phase_ = Phase::IDLE;

    // Deploy-scope state (lives across all padawans of one
    // FW_DEPLOY_BEGIN).
    std::string deployMsgId_;
    std::string deployTransferId_;
    std::vector<std::string> orderList_;
    size_t nextOrderIdx_ = 0;
    std::vector<PadawanResult> results_;

    // Per-padawan state (lives across one padawan's transfer; reset on
    // advance).
    std::string currentControllerId_;
    uint8_t currentPadawanMac_[6] = {0};
    uint8_t currentXferId_ = 0;
    FILE *firmwareFile_ = nullptr;
    uint32_t firmwareTotalSize_ = 0;
    uint32_t firmwareTotalChunks_ = 0;
    uint8_t firmwareSha256_[32] = {0};

    // Stats counters (reset in startNextPadawan). All read+written by the
    // owning task only (otaForwarderTask) — no atomics. lastSentSeq_ is the
    // high-water mark of seqs placed on the wire — retransmits don't refresh
    // it, so this reflects forward progress, not the latest retry.
    // highestAckedSeq_ tracks the cumulative-ACK cursor from handleDataAck.
    uint32_t statsLastSentSeq_ = 0;
    uint32_t statsHighestAckedSeq_ = 0;
    bool statsAnyAcked_ = false; // disambiguates "0 acked" from "none acked yet"
    uint32_t statsNaksRecvCount_ = 0;
    uint32_t statsSendFailCount_ = 0;

    // FW_PROGRESS SENDING throttle: emit on every >=5% byte advance.
    // Reset to 0 in startNextPadawan; updated in streamDrain.
    uint32_t lastProgressBytesSent_ = 0;
};

extern OtaForwarder AstrOs_OtaForwarder;

#endif

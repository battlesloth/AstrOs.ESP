#ifndef OTARECEIVER_HPP
#define OTARECEIVER_HPP

#include <AstrOsBulkTransport.hpp>
#include <OtaQueueMessage.h>

#include <cstdint>
#include <string>

// needed for QueueHandle_t, must be in this order
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <esp_timer.h>

// Threading: all members of this class are accessed only from otaReceiverTask
// via `process(...)`. No synchronization is required for this class's own
// state. The handler implementations call `AstrOs_SerialMsgHandler.sendFw*(...)`
// to emit FW_*_ACK / NAK / DONE replies; that call terminates in xQueueSend
// against serialQueue and is independently thread-safe.
//
// The idle watchdog timer fires from esp_timer's dispatch task (NOT
// otaReceiverTask), but its callback only does an `xQueueSend` of an
// OTA_MSG_WATCHDOG_FIRE message — the actual state mutation runs on
// otaReceiverTask via `process(...)`, preserving the single-task-state
// invariant above.
class OtaReceiver
{
private:
    AstrOsBulkTransport::BulkReceiver bulk_;
    bool active_ = false;
    // Echoed back in every FW_*_ACK / NAK on this transfer.
    std::string transferIdStr_;
    // BEGIN-time msgId; END's own msgId echoes via the END record itself.
    std::string beginMsgId_;

    // Idle-activity watchdog. Started at end of handleBegin, restarted at
    // end of handleChunk, stopped at end of handleEnd. Fires if no chunk
    // activity for `kWatchdogIdleUs` microseconds — meaning a transfer
    // got stuck after BEGIN (server crash, link drop, all-chunks-rejected).
    // Callback posts OTA_MSG_WATCHDOG_FIRE to otaQueue_ for in-task abort.
    esp_timer_handle_t watchdog_ = nullptr;
    QueueHandle_t otaQueue_ = nullptr;
    static constexpr uint64_t kWatchdogIdleUs = 10ULL * 1000ULL * 1000ULL; // 10 s

public:
    OtaReceiver();
    ~OtaReceiver();

    // Split from the constructor so the global can be constructed at
    // static-init time, before FreeRTOS queues exist. `otaQueue` is held
    // so the watchdog callback can post abort messages back into the same
    // queue otaReceiverTask is draining.
    void Init(QueueHandle_t otaQueue);

    // Frees every malloc'd pointer in the union arm matching msg.kind.
    void process(queue_ota_msg_t &msg);

private:
    void handleBegin(queue_ota_msg_t &msg);
    void handleChunk(queue_ota_msg_t &msg);
    void handleEnd(queue_ota_msg_t &msg);
    void handleDeployBegin(queue_ota_msg_t &msg);
    void handleWatchdogFire();

    // Timer helpers — wrap esp_timer_start_once + esp_timer_stop so the
    // call sites in handleBegin/handleChunk/handleEnd stay one-liners and
    // the "stop-then-start" restart pattern is centralized.
    void watchdogStart();
    void watchdogRestart();
    void watchdogStop();

    // esp_timer callback indirection — the timer's `arg` is `this`, so the
    // callback can resolve the instance and call into the queue.
    static void watchdogTimerCb(void *arg);
};

extern OtaReceiver AstrOs_OtaReceiver;

#endif

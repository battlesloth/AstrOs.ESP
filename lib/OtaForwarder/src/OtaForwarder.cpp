#include "OtaForwarder.hpp"

#include <AstrOsSerialMsgHandler.hpp>
#include <esp_log.h>

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

// Task 7a/7b fill in the bodies. Stubs for now so the firmware builds.
void OtaForwarder::handleDeployBegin(queue_ota_forwarder_msg_t &msg)
{
    ESP_LOGW(TAG, "handleDeployBegin: stubbed (Task 7a)");
    (void)msg;
}
void OtaForwarder::handleBeginAck(queue_ota_forwarder_msg_t &msg)
{
    ESP_LOGW(TAG, "handleBeginAck: stubbed (Task 7a)");
    (void)msg;
}
void OtaForwarder::handleBeginNak(queue_ota_forwarder_msg_t &msg)
{
    ESP_LOGW(TAG, "handleBeginNak: stubbed (Task 7a)");
    (void)msg;
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
    ESP_LOGW(TAG, "startNextPadawan: stubbed (Task 7a)");
}
void OtaForwarder::abortCurrentPadawan(const std::string &reason)
{
    ESP_LOGW(TAG, "abortCurrentPadawan(%s): stubbed (Task 7a)", reason.c_str());
}
void OtaForwarder::completeCurrentPadawan()
{
    ESP_LOGW(TAG, "completeCurrentPadawan: stubbed (Task 7b)");
}
void OtaForwarder::emitDeployDoneAndReset()
{
    ESP_LOGW(TAG, "emitDeployDoneAndReset: stubbed (Task 7a)");
}

void OtaForwarder::emitOtaBeginFrame()
{
    ESP_LOGW(TAG, "emitOtaBeginFrame: stubbed (Task 7a)");
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
    (void)arg;
    // Stubbed in this skeleton; Task 7a posts a synthetic timeout event
    // by triggering a NAK-equivalent path. For now the timer just fires
    // and the stubbed handlers ignore it.
}

void OtaForwarder::endAckTimerCb(void *arg)
{
    (void)arg;
    // See beginAckTimerCb note. Task 7b implements.
}

bool OtaForwarder::resolveControllerMac(const std::string &controllerId, uint8_t outMac[6]) const
{
    ESP_LOGW(TAG, "resolveControllerMac(%s): stubbed (Task 7a)", controllerId.c_str());
    (void)outMac;
    return false;
}

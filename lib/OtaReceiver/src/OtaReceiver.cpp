#include <OtaReceiver.hpp>

#include <esp_log.h>

static const char *TAG = "OtaReceiver";

OtaReceiver AstrOs_OtaReceiver;

OtaReceiver::OtaReceiver() {}

OtaReceiver::~OtaReceiver() {}

void OtaReceiver::Init()
{
    // Phase 3: nothing to do here yet. Phase 4 will allocate the SHA
    // context and ensure the firmware staging dir exists on SD.
    ESP_LOGI(TAG, "OtaReceiver initialized");
}

void OtaReceiver::process(queue_ota_msg_t &msg)
{
    switch (msg.kind)
    {
    case OTA_MSG_BEGIN:
        handleBegin(msg);
        break;
    case OTA_MSG_CHUNK:
        handleChunk(msg);
        break;
    case OTA_MSG_END:
        handleEnd(msg);
        break;
    case OTA_MSG_DEPLOY_BEGIN:
        handleDeployBegin(msg);
        break;
    default:
        ESP_LOGE(TAG, "Unknown ota_msg_kind_t: %d", static_cast<int>(msg.kind));
        free(msg.transferId);
        break;
    }
}

void OtaReceiver::handleBegin(queue_ota_msg_t &msg)
{
    // Task 3 implements this.
    free(msg.begin.msgId);
    free(msg.begin.targetList);
    free(msg.transferId);
}

void OtaReceiver::handleChunk(queue_ota_msg_t &msg)
{
    // Task 4 implements this.
    free(msg.chunk.payload);
    free(msg.transferId);
}

void OtaReceiver::handleEnd(queue_ota_msg_t &msg)
{
    // Task 5 implements this.
    free(msg.end.msgId);
    free(msg.transferId);
}

void OtaReceiver::handleDeployBegin(queue_ota_msg_t &msg)
{
    // Task 6 implements this.
    free(msg.deploy.msgId);
    free(msg.deploy.orderList);
    free(msg.transferId);
}

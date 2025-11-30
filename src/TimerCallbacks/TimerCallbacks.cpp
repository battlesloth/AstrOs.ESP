#include <TimerCallbacks/TimerCallbacks.hpp>

#include <esp_log.h>
#include <string.h>

#include <AstrOsUtility_Esp.h>
#include <AstrOsEspNow.h>
#include <AstrOsStorageManager.hpp>
#include <AstrOsSerialMsgHandler.hpp>
#include <AstrOsDisplay.hpp>

static const char *TAG = "TimerCallbacks";

void pollingCallback(
    bool isMasterNode,
    bool discoveryMode,
    QueueHandle_t espnowQueue,
    bool &polling,
    int &displayTimeout)
{
    // only send register requests during discovery mode
    if (isMasterNode && !discoveryMode)
    {
        queue_espnow_msg_t msg;
        msg.data = nullptr;

        if (!polling)
        {
            msg.eventType = EspNowQueueEventType::POLL_PADAWANS;
            polling = true;

            char *fingerprint = (char *)malloc(37);
            AstrOs_Storage.getControllerFingerprint(fingerprint);
            AstrOs_SerialMsgHandler.sendPollAckNak("00:00:00:00:00:00", "master",
                                                   std::string(fingerprint), true);
        }
        else
        {
            msg.eventType = EXPIRE_POLLS;
            polling = false;
        }

        if (xQueueSend(espnowQueue, &msg, pdMS_TO_TICKS(250)) != pdTRUE)
        {
            ESP_LOGW(TAG, "Send espnow queue fail");
        }
    }
    else if (!isMasterNode && discoveryMode)
    {
        queue_espnow_msg_t msg;
        msg.data = nullptr;

        msg.eventType = SEND_REGISTRAION_REQ;

        if (xQueueSend(espnowQueue, &msg, pdMS_TO_TICKS(250)) != pdTRUE)
        {
            ESP_LOGW(TAG, "Send espnow queue fail");
        }
    }

    if (!discoveryMode && displayTimeout > 0)
    {
        ESP_LOGD(TAG, "Display Timeout: %d", displayTimeout);
        displayTimeout -= 2;
        if (displayTimeout <= 0)
        {
            AstrOs_Display.displayClear();
        }
    }
};

void animationCallback(
    CommandTemplate* cmd,
    QueueHandle_t serialCh1Queue,
    QueueHandle_t serialCh2Queue,
    QueueHandle_t servoQueue,
    QueueHandle_t i2cQueue,
    QueueHandle_t gpioQueue)
{
    if (cmd == nullptr)
    {
        ESP_LOGE(TAG, "Annimation Command pointer is null");
        return;
    }

    MODULE_TYPE ct = cmd->type;
    std::string val = cmd->val;
    int module = cmd->module;

    switch (ct)
    {
    case MODULE_TYPE::NONE:
    {
        ESP_LOGI(TAG, "NONE command queued, assume buffer?");
        break;
    }
    case MODULE_TYPE::KANGAROO:
    case MODULE_TYPE::GENERIC_SERIAL:
    {
        ESP_LOGI(TAG, "Serial command val: %s", val.c_str());
        queue_serial_msg_t serialMsg;
        serialMsg.message_id = 0;
        serialMsg.data = (uint8_t *)malloc(val.size() + 1);
        memcpy(serialMsg.data, val.c_str(), val.size());
        serialMsg.data[val.size()] = '\0';

        if (module == 1)
        {
            if (xQueueSend(serialCh1Queue, &serialMsg, pdMS_TO_TICKS(2000)) != pdTRUE)
            {
                ESP_LOGW(TAG, "Send serial queue fail");
                free(serialMsg.data);
            }
        }
        else if (module == 2)
        {
            if (xQueueSend(serialCh2Queue, &serialMsg, pdMS_TO_TICKS(2000)) != pdTRUE)
            {
                ESP_LOGW(TAG, "Send serial queue fail");
                free(serialMsg.data);
            }
        }
        else
        {
            ESP_LOGE(TAG, "Invalid serial module %d", module);
            free(serialMsg.data);
        }
        break;
    }
    case MODULE_TYPE::MAESTRO:
    {
        ESP_LOGI(TAG, "Maestro command val: %s", val.c_str());
        queue_msg_t servoMsg;
        servoMsg.message_id = module;
        servoMsg.data = (uint8_t *)malloc(val.size() + 1);
        memcpy(servoMsg.data, val.c_str(), val.size());
        servoMsg.data[val.size()] = '\0';

        if (xQueueSend(servoQueue, &servoMsg, pdMS_TO_TICKS(2000)) != pdTRUE)
        {
            ESP_LOGW(TAG, "Send servo queue fail");
            free(servoMsg.data);
        }
        break;
    }
    case MODULE_TYPE::I2C:
    {
        ESP_LOGI(TAG, "I2C command val: %s", val.c_str());
        queue_msg_t i2cMsg;
        i2cMsg.message_id = 0;
        i2cMsg.data = (uint8_t *)malloc(val.size() + 1);
        memcpy(i2cMsg.data, val.c_str(), val.size());
        i2cMsg.data[val.size()] = '\0';

        if (xQueueSend(i2cQueue, &i2cMsg, pdMS_TO_TICKS(2000)) != pdTRUE)
        {
            ESP_LOGW(TAG, "Send i2c queue fail");
            free(i2cMsg.data);
        }
        break;
    }
    case MODULE_TYPE::GPIO:
    {
        ESP_LOGI(TAG, "GPIO command val: %s", val.c_str());
        queue_msg_t gpioMsg;
        gpioMsg.data = (uint8_t *)malloc(val.size() + 1);
        memcpy(gpioMsg.data, val.c_str(), val.size());
        gpioMsg.data[val.size()] = '\0';

        if (xQueueSend(gpioQueue, &gpioMsg, pdMS_TO_TICKS(2000)) != pdTRUE)
        {
            ESP_LOGW(TAG, "Send gpio queue fail");
            free(gpioMsg.data);
        }
        break;
    }
    default:
        break;
    }
}
#include "AstrOsDisplay.hpp"
#include <AstrOsUtility.h>
#include <string.h>
#include <esp_log.h>

static const char *TAG = "AstrOsDisplay";

AstrOsDisplayService::AstrOsDisplayService()
{
}

AstrOsDisplayService::~AstrOsDisplayService()
{
}

void AstrOsDisplayService::init(QueueHandle_t i2cQueue)
{
    AstrOsDisplayService::i2cQqueue = i2cQueue;
}

void AstrOsDisplayService::setDefault(std::string line1, std::string line2, std::string line3)
{
    AstrOsDisplayService::defaultLine1 = line1;
    AstrOsDisplayService::defaultLine2 = line2;
    AstrOsDisplayService::defaultLine3 = line3;
}

void AstrOsDisplayService::displayDefault()
{
    AstrOsDisplayService::displayUpdate(defaultLine1, defaultLine2, defaultLine3);
}

void AstrOsDisplayService::displayUpdate(std::string line1, std::string line2, std::string line3)
{
    DisplayCommand cmd;

    if (line2.empty() && line3.empty())
    {
        cmd.setValue("", line1, "");
    }
    else if (line3.empty())
    {
        cmd.setValue(line1, "", line2);
    }
    else
    {
        cmd.setValue(line1, line2, line3);
    }

    auto strValue = cmd.toString();
    queue_msg_t i2cMsg;
    i2cMsg.message_id = 1;
    i2cMsg.data = (uint8_t *)malloc(strValue.length() + 1);
    memcpy(i2cMsg.data, strValue.c_str(), strValue.length());
    i2cMsg.data[strValue.length()] = '\0';

    if (xQueueSend(AstrOsDisplayService::i2cQqueue, &i2cMsg, pdMS_TO_TICKS(100)) == pdFALSE)
    {
        ESP_LOGE(TAG, "Failed to send display command to hardware queue");
    }
}

void AstrOsDisplayService::displayClear()
{
    DisplayCommand cmd;

    cmd.setValue("", "", "");

    auto strValue = cmd.toString();
    queue_msg_t i2cMsg;
    i2cMsg.message_id = 1;
    i2cMsg.data = (uint8_t *)malloc(strValue.length() + 1);
    memcpy(i2cMsg.data, strValue.c_str(), strValue.length());
    i2cMsg.data[strValue.length()] = '\0';

    if (xQueueSend(AstrOsDisplayService::i2cQqueue, &i2cMsg, pdMS_TO_TICKS(100)) == pdFALSE)
    {
        ESP_LOGE(TAG, "Failed to send display clear command to hardware queue");
    }
}

AstrOsDisplayService AstrOs_Display = AstrOsDisplayService();
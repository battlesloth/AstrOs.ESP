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

void AstrOsDisplayService::init(QueueHandle_t hardwareQueue)
{
    AstrOsDisplayService::hardwareQueue = hardwareQueue;
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

    queue_hw_cmd_t msg = {HARDWARE_COMMAND::DISPLAY_COMMAND, '0'};
    strncpy(msg.data, cmd.toString().c_str(), sizeof(msg.data));
    msg.data[sizeof(msg.data) - 1] = '\0';
    if (xQueueSend(AstrOsDisplayService::hardwareQueue, &msg, pdMS_TO_TICKS(100)) == pdFALSE)
    {
        ESP_LOGE(TAG, "Failed to send display command to hardware queue");
    }
}

AstrOsDisplayService AstrOs_Display = AstrOsDisplayService();
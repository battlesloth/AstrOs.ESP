#include "AstrOsDisplay.hpp"
#include <AstrOsUtility.h>
#include <string.h>

QueueHandle_t hardwareQueue;

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

void AstrOsDisplayService::displayUpdate(std::string line1, std::string line2, std::string line3)
{

    DisplayCommand cmd;

    if (line2.empty())
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

    queue_hw_cmd_t msg = {HARDWARE_COMMAND::DISPLAY_COMMAND, NULL};
    strncpy(msg.data, cmd.toString().c_str(), sizeof(msg.data));
    msg.data[sizeof(msg.data) - 1] = '\0';
    xQueueSend(hardwareQueue, &cmd, 0);
}

AstrOsDisplayService AstrOs_Display = AstrOsDisplayService();
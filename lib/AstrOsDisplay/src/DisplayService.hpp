#ifndef DISPLAYSERVICE_HPP
#define DISPLAYSERVICE_HPP

// needed for QueueHandle_t, must be in this order
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string>

class AstrOsDisplayService
{
private:
    QueueHandle_t hardwareQueue;

public:
    AstrOsDisplayService(/* args */);
    ~AstrOsDisplayService();
    void init(QueueHandle_t hardwareQueue);
    void displayUpdate(std::string line1, std::string line2 = "", std::string line3 = "");
};

extern AstrOsDisplayService AstrOs_Display;

#endif
#ifndef DISPLAYSERVICE_HPP
#define DISPLAYSERVICE_HPP

// needed for QueueHandle_t, must be in this order
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string>

class AstrOsDisplayService
{
private:
    QueueHandle_t i2cQqueue;
    std::string defaultLine1;
    std::string defaultLine2;
    std::string defaultLine3;

public:
    AstrOsDisplayService(/* args */);
    ~AstrOsDisplayService();
    void init(QueueHandle_t i2cQueue);
    void setDefault(std::string line1, std::string line2, std::string line3);
    void displayDefault();
    void displayUpdate(std::string line1, std::string line2 = "", std::string line3 = "");
};

extern AstrOsDisplayService AstrOs_Display;

#endif
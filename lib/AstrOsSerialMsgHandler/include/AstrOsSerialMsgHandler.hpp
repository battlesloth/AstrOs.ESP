#ifndef ASTROSSERIALMSGHANDLER_HPP
#define ASTROSSERIALMSGHANDLER_HPP

#include <string>
// needed for QueueHandle_t, must be in this order
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

class AstrOsSerialMsgHandler
{
private:
    QueueHandle_t serverResponseQueue;

public:
    AstrOsSerialMsgHandler();
    ~AstrOsSerialMsgHandler();
    void Init(QueueHandle_t serverResponseQueue);
    void handleMessage(std::string message);
};

extern AstrOsSerialMsgHandler AstrOs_SerialMsgHandler;

#endif
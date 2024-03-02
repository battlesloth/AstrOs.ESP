#ifndef ASTROSSERIALMSGHANDLER_HPP
#define ASTROSSERIALMSGHANDLER_HPP

#include <AstrOsMessaging.hpp>

#include <string>
// needed for QueueHandle_t, must be in this order
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

class AstrOsSerialMsgHandler
{
private:
    QueueHandle_t handlerQueue;
    QueueHandle_t serialQueue;
    void handleRegistrationSync(std::string msgId);
    void handleDeployConfig(std::string msgId, std::string message);

public:
    AstrOsSerialMsgHandler();
    ~AstrOsSerialMsgHandler();
    void Init(QueueHandle_t serverResponseQueue, QueueHandle_t serialQueue);
    void handleMessage(std::string message);
    void sendBasicAckNakResponse(AstrOsSerialMessageType type, std::string msgId, std::string mac, std::string name, std::string payload);
};

extern AstrOsSerialMsgHandler AstrOs_SerialMsgHandler;

#endif
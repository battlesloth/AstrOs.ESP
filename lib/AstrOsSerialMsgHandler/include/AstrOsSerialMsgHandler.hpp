#ifndef ASTROSSERIALMSGHANDLER_HPP
#define ASTROSSERIALMSGHANDLER_HPP

#include <AstrOsMessaging.hpp>
#include <AstrOsInterfaceResponseMsg.hpp>

#include <string>
#include <vector>
// needed for QueueHandle_t, must be in this order
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

class AstrOsSerialMsgHandler
{
private:
    QueueHandle_t handlerQueue;
    QueueHandle_t serialQueue;

    AstrOsSerialMessageService msgService;

    void handleRegistrationSync(std::string msgId);
    void handleDeployConfig(std::string msgId, std::string message);
    void handleDeployScript(std::string msgId, std::string message);

    void handleBasicCommand(AstrOsSerialMessageType type, std::string msgId, std::string message);

    AstrOsInterfaceResponseType getResponseType(AstrOsSerialMessageType type, bool isMaster);
    void sendToInterfaceQueue(AstrOsInterfaceResponseType responseType, std::string msgId,
                              std::string peerMac, std::string peerName, std::string message);

public:
    AstrOsSerialMsgHandler();
    ~AstrOsSerialMsgHandler();
    void Init(QueueHandle_t serverResponseQueue, QueueHandle_t serialQueue);
    void handleMessage(std::string message);
    void sendRegistraionAck(std::string msgId, std::vector<astros_peer_data_t> peers);
    void sendPollAckNak(std::string mac, std::string name, std::string fingerprint, bool isAck);
    void sendBasicAckNakResponse(AstrOsSerialMessageType type, std::string msgId, std::string mac, std::string name, std::string payload);
};

extern AstrOsSerialMsgHandler AstrOs_SerialMsgHandler;

#endif
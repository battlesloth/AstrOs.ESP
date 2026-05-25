#ifndef ASTROSSERIALMSGHANDLER_HPP
#define ASTROSSERIALMSGHANDLER_HPP

#include <AstrOsInterfaceResponseMsg.hpp>
#include <AstrOsMessaging.hpp>

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
    QueueHandle_t otaQueue;
    QueueHandle_t otaForwarderQueue;

    AstrOsSerialMessageService msgService;

    void sendToInterfaceQueue(AstrOsInterfaceResponseType responseType, std::string msgId, std::string peerMac,
                              std::string peerName, std::string message);

    void handleFwTransferBeginInbound(const std::string &msgId, const std::string &payload);
    void handleFwChunkInbound(const std::string &payload);
    void handleFwTransferEndInbound(const std::string &msgId, const std::string &payload);
    void handleFwDeployBeginInbound(const std::string &msgId, const std::string &payload);

public:
    AstrOsSerialMsgHandler();
    ~AstrOsSerialMsgHandler();
    void Init(QueueHandle_t serverResponseQueue, QueueHandle_t serialQueue, QueueHandle_t otaQueue,
              QueueHandle_t otaForwarderQueue);
    void handleMessage(std::string message);
    void sendRegistraionAck(std::string msgId, std::vector<astros_peer_data_t> peers);
    void sendPollAckNak(std::string mac, std::string name, std::string fingerprint, std::string firmwareVersion,
                        std::string variant, bool isAck);
    void sendBasicAckNakResponse(AstrOsSerialMessageType type, std::string msgId, std::string mac, std::string name,
                                 std::string payload);

    void sendFwTransferBeginAck(std::string msgId, std::string transferId, std::string status);
    void sendFwChunkAck(std::string transferId, uint32_t highestContiguousSeq, uint32_t nextExpectedSeq,
                        uint8_t windowRemaining);
    void sendFwChunkNak(std::string transferId, uint32_t lastGoodSeq, uint32_t nextExpectedSeq, std::string reasonCode);
    void sendFwTransferEndAck(std::string msgId, std::string transferId, std::string status,
                              std::string computedSha256Hex);
    void sendFwDeployDone(std::string msgId, std::string transferId, std::vector<astros_fw_deploy_result_t> results);
};

extern AstrOsSerialMsgHandler AstrOs_SerialMsgHandler;

#endif

#include <AstrOsSerialMsgHandler.hpp>
#include <AstrOsInterfaceResponseMsg.hpp>
#include <AstrOsMessaging.hpp>
#include <AstrOsStringUtils.hpp>
#include <AstrOsUtility.h>
#include <esp_log.h>
#include <string.h>

const char *TAG = "AstrOsSerialMsgHandler";

AstrOsSerialMsgHandler AstrOs_SerialMsgHandler;

AstrOsSerialMsgHandler::AstrOsSerialMsgHandler() {}

AstrOsSerialMsgHandler::~AstrOsSerialMsgHandler() {}

void AstrOsSerialMsgHandler::Init(QueueHandle_t handlerQueue, QueueHandle_t serialQueue)
{
    this->handlerQueue = handlerQueue;
    this->serialQueue = serialQueue;

    this->msgService = AstrOsSerialMessageService();
}

void AstrOsSerialMsgHandler::handleMessage(std::string message)
{
    auto validation = this->msgService.validateSerialMsg(message);

    if (!validation.valid)
    {
        ESP_LOGE(TAG, "Invalid message: %s", message.c_str());
        return;
    }

    switch (validation.type)
    {
    case AstrOsSerialMessageType::REGISTRATION_SYNC:
    {
        this->handleRegistrationSync(validation.msgId);
        break;
    }
    case AstrOsSerialMessageType::DEPLOY_CONFIG:
        this->handleDeployConfig(validation.msgId, message);
        break;
    case AstrOsSerialMessageType::DEPLOY_SCRIPT:
        this->handleDeployScript(validation.msgId, message);
        break;
    case AstrOsSerialMessageType::RUN_SCRIPT:
    case AstrOsSerialMessageType::PANIC_STOP:
    case AstrOsSerialMessageType::FORMAT_SD:
    case AstrOsSerialMessageType::RUN_COMMAND:
        this->handleBasicCommand(validation.type, validation.msgId, message);
        break;
    default:
        ESP_LOGE(TAG, "Unknown/Invalid message type: %d", static_cast<int>(validation.type));
        break;
    }
}

void AstrOsSerialMsgHandler::handleRegistrationSync(std::string msgId)
{
    ESP_LOGD(TAG, "Received REGISTRATION_SYNC message");

    this->sendToInterfaceQueue(AstrOsInterfaceResponseType::REGISTRATION_SYNC, msgId, "", "", "");
}

void AstrOsSerialMsgHandler::handleDeployConfig(std::string msgId, std::string message)
{
    ESP_LOGD(TAG, "Received DEPLOY_CONFIG message");

    auto parts = AstrOsStringUtils::splitString(message, GROUP_SEPARATOR);
    auto controllers = AstrOsStringUtils::splitString(parts[1], RECORD_SEPARATOR);

    for (auto controller : controllers)
    {
        auto msgParts = AstrOsStringUtils::splitString(controller, UNIT_SEPARATOR);
        if (msgParts.size() != 4)
        {
            ESP_LOGE(TAG, "Invalid config: %s", controller.c_str());
            continue;
        }

        if (msgParts[0] == "00:00:00:00:00:00")
        {
            auto responseType = this->getResponseType(AstrOsSerialMessageType::DEPLOY_CONFIG, true);
            this->sendToInterfaceQueue(responseType, msgId, "", "", msgParts[3]);
        }
        else
        {
            auto responseType = this->getResponseType(AstrOsSerialMessageType::DEPLOY_CONFIG, false);
            this->sendToInterfaceQueue(responseType, msgId, msgParts[0], "", msgParts[3]);
        }
    }
}

void AstrOsSerialMsgHandler::handleDeployScript(std::string msgId, std::string message)
{
    ESP_LOGD(TAG, "Received DEPLOY_SCRIPT message");

    auto parts = AstrOsStringUtils::splitString(message, GROUP_SEPARATOR);
    auto controllers = AstrOsStringUtils::splitString(parts[1], RECORD_SEPARATOR);

    for (auto controller : controllers)
    {
        auto msgParts = AstrOsStringUtils::splitString(controller, UNIT_SEPARATOR);
        if (msgParts.size() != 4)
        {
            ESP_LOGE(TAG, "Invalid script: %s", controller.c_str());
            continue;
        }

        auto script = msgParts[2] + UNIT_SEPARATOR + msgParts[3];

        if (msgParts[0] == "00:00:00:00:00:00")
        {
            auto responseType = this->getResponseType(AstrOsSerialMessageType::DEPLOY_SCRIPT, true);
            this->sendToInterfaceQueue(responseType, msgId, "", "", script);
        }
        else
        {
            auto responseType = this->getResponseType(AstrOsSerialMessageType::DEPLOY_SCRIPT, false);
            this->sendToInterfaceQueue(responseType, msgId, msgParts[0], "", script);
        }
    }
}

/// @brief Handles basic serial commands. Basic serial commands are commands that have a
///        simple stucture of Address|Controller|Command and and don't require command formatting.
/// @param msgId
/// @param message
void AstrOsSerialMsgHandler::handleBasicCommand(AstrOsSerialMessageType type, std::string msgId, std::string message)
{
    ESP_LOGD(TAG, "Received message type: %d", static_cast<int>(type));

    auto parts = AstrOsStringUtils::splitString(message, GROUP_SEPARATOR);
    auto controllers = AstrOsStringUtils::splitString(parts[1], RECORD_SEPARATOR);

    for (auto controller : controllers)
    {
        auto msgParts = AstrOsStringUtils::splitString(controller, UNIT_SEPARATOR);

        if (msgParts.size() != 3)
        {
            ESP_LOGE(TAG, "Invalid command: %s", controller.c_str());
            continue;
        }

        if (msgParts[0].empty())
        {
            ESP_LOGW(TAG, "Empty command destination: %s", controller.c_str());
            continue;
        }

        if (msgParts[2].empty())
        {
            ESP_LOGW(TAG, "Empty command value: %s", controller.c_str());
            continue;
        }

        if (msgParts[0] == "00:00:00:00:00:00")
        {
            auto responseType = this->getResponseType(type, true);
            this->sendToInterfaceQueue(responseType, msgId, "", "", msgParts[2]);
        }
        else
        {
            auto responseType = this->getResponseType(type, false);
            this->sendToInterfaceQueue(responseType, msgId, msgParts[0], "", msgParts[2]);
        }
    }
}

AstrOsInterfaceResponseType AstrOsSerialMsgHandler::getResponseType(AstrOsSerialMessageType type, bool isMaster)
{
    if (isMaster)
    {
        switch (type)
        {
        case AstrOsSerialMessageType::REGISTRATION_SYNC:
            return AstrOsInterfaceResponseType::REGISTRATION_SYNC;
        case AstrOsSerialMessageType::DEPLOY_CONFIG:
            return AstrOsInterfaceResponseType::SET_CONFIG;
        case AstrOsSerialMessageType::DEPLOY_SCRIPT:
            return AstrOsInterfaceResponseType::SAVE_SCRIPT;
        case AstrOsSerialMessageType::RUN_SCRIPT:
            return AstrOsInterfaceResponseType::SCRIPT_RUN;
        default:
            return AstrOsInterfaceResponseType::UNKNOWN;
        }
    }
    else
    {
        switch (type)
        {
        case AstrOsSerialMessageType::DEPLOY_CONFIG:
            return AstrOsInterfaceResponseType::SEND_CONFIG;
        case AstrOsSerialMessageType::DEPLOY_SCRIPT:
            return AstrOsInterfaceResponseType::SEND_SCRIPT;
        case AstrOsSerialMessageType::RUN_SCRIPT:
            return AstrOsInterfaceResponseType::SEND_SCRIPT_RUN;
        default:
            return AstrOsInterfaceResponseType::UNKNOWN;
        }
    }
}

/************************************
 * Send methods
 *************************************/

void AstrOsSerialMsgHandler::sendToInterfaceQueue(AstrOsInterfaceResponseType responseType, std::string msgId,
                                                  std::string peerMac, std::string peerName, std::string message)
{
    auto msgIdSize = msgId.size() + 1;
    auto peerMacSize = peerMac.size() + 1;
    auto peerNameSize = peerName.size() + 1;
    auto messageSize = message.size() + 1;

    astros_interface_response_t response;
    response.type = responseType;
    response.originationMsgId = msgIdSize == 1 ? nullptr : (char *)malloc(msgIdSize);
    response.peerMac = peerMacSize == 1 ? nullptr : (char *)malloc(peerMacSize);
    response.peerName = peerNameSize == 1 ? nullptr : (char *)malloc(peerNameSize);
    response.message = messageSize == 1 ? nullptr : (char *)malloc(messageSize);

    if (msgIdSize > 1)
    {
        memcpy(response.originationMsgId, msgId.c_str(), msgIdSize);
    }
    if (peerMacSize > 1)
    {
        memcpy(response.peerMac, peerMac.c_str(), peerMacSize);
    }
    if (peerNameSize > 1)
    {
        memcpy(response.peerName, peerName.c_str(), peerNameSize);
    }
    if (messageSize > 1)
    {
        memcpy(response.message, message.c_str(), messageSize);
    }

    if (xQueueSend(this->handlerQueue, &response, pdTICKS_TO_MS(250)) == pdFALSE)
    {
        ESP_LOGE(TAG, "Failed to send message to interface handler queue, type: %d", static_cast<int>(responseType));
        free(response.originationMsgId);
        free(response.peerMac);
        free(response.peerName);
        free(response.message);
    }
}

void AstrOsSerialMsgHandler::sendRegistraionAck(std::string msgId, std::vector<astros_peer_data_t> data)
{
    auto response = this->msgService.getRegistrationSyncAck(msgId, data);

    ESP_LOGD(TAG, "Sending registraion ack: %s", response.c_str());

    queue_msg_t serialMsg;

    serialMsg.message_id = 1;
    serialMsg.data = (uint8_t *)malloc(response.size() + 1);
    memcpy(serialMsg.data, response.c_str(), response.size());
    serialMsg.data[response.size()] = '\n';
    serialMsg.dataSize = response.size() + 1;

    if (xQueueSend(serialQueue, &serialMsg, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Send serial queue fail");
        free(serialMsg.data);
    }
}

void AstrOsSerialMsgHandler::sendPollAckNak(std::string mac, std::string name, std::string fingerprint, bool isAck)
{
    std::string response;

    if (isAck)
    {
        response = this->msgService.getPollAck(mac, name, fingerprint);
    }
    else
    {
        response = this->msgService.getPollNak(mac, name);
    }

    queue_msg_t serialMsg;

    serialMsg.message_id = 1;
    serialMsg.data = (uint8_t *)malloc(response.size() + 1);
    memcpy(serialMsg.data, response.c_str(), response.size());
    serialMsg.data[response.size()] = '\n';
    serialMsg.dataSize = response.size() + 1;

    if (xQueueSend(serialQueue, &serialMsg, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Send serial queue fail");
        free(serialMsg.data);
    }
}

void AstrOsSerialMsgHandler::sendBasicAckNakResponse(AstrOsSerialMessageType type, std::string msgId, std::string mac, std::string name, std::string payload)
{
    auto response = this->msgService.getBasicAckNak(type, msgId, mac, name, payload);

    ESP_LOGD(TAG, "Sending response: %s", response.c_str());

    queue_msg_t serialMsg;

    serialMsg.message_id = 1;
    serialMsg.data = (uint8_t *)malloc(response.size() + 1);
    memcpy(serialMsg.data, response.c_str(), response.size());
    serialMsg.data[response.size()] = '\n';
    serialMsg.dataSize = response.size() + 1;

    if (xQueueSend(serialQueue, &serialMsg, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Send serial queue fail");
        free(serialMsg.data);
    }
}
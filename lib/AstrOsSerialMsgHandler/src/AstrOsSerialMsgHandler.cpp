
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
}

void AstrOsSerialMsgHandler::handleMessage(std::string message)
{

    auto validation = AstrOsSerialMessageService::validateSerialMsg(message);

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
        ESP_LOGI(TAG, "Received DEPLOY_SCRIPT message");
        break;
    case AstrOsSerialMessageType::RUN_SCRIPT:
        ESP_LOGI(TAG, "Received RUN_SCRIPT message");
        break;
    case AstrOsSerialMessageType::RUN_COMMAND:
        ESP_LOGI(TAG, "Received RUN_COMMAND message");
        break;
    default:
        ESP_LOGE(TAG, "Unknown/Invalid message type: %d", static_cast<int>(validation.type));
        break;
    }
}

void AstrOsSerialMsgHandler::handleRegistrationSync(std::string msgId)
{
    ESP_LOGD(TAG, "Received REGISTRATION_SYNC message");

    auto msgIdSize = msgId.size() + 1;

    astros_interface_response_t response = {
        .type = AstrOsInterfaceResponseType::REGISTRATION_SYNC,
        .originationMsgId = (char *)malloc(msgIdSize),
        .peerMac = nullptr,
        .peerName = nullptr,
        .message = nullptr};

    memcpy(response.originationMsgId, msgId.c_str(), msgIdSize);

    if (xQueueSend(this->handlerQueue, &response, pdTICKS_TO_MS(250)) == pdFALSE)
    {
        ESP_LOGE(TAG, "Failed to send message to handler queue");
    }
}

void AstrOsSerialMsgHandler::handleDeployConfig(std::string msgId, std::string message)
{
    ESP_LOGD(TAG, "Received DEPLOY_CONFIG message");

    auto parts = AstrOsStringUtils::splitString(message, GROUP_SEPARATOR);
    auto controllers = AstrOsStringUtils::splitString(parts[1], RECORD_SEPARATOR);

    auto msgIdSize = msgId.size() + 1;

    for (auto controller : controllers)
    {
        auto msgParts = AstrOsStringUtils::splitString(controller, UNIT_SEPARATOR);
        if (msgParts.size() != 4)
        {
            ESP_LOGE(TAG, "Invalid config: %s", controller.c_str());
            continue;
        }

        auto peerSize = msgParts[0].size() + 1;
        auto configSize = msgParts[3].size() + 1;

        if (msgParts[0] == "00:00:00:00:00:00")
        {
            astros_interface_response_t response = {
                .type = AstrOsInterfaceResponseType::SET_CONFIG,
                .originationMsgId = (char *)malloc(msgIdSize),
                .peerMac = nullptr,
                .peerName = nullptr,
                .message = (char *)malloc(configSize)};

            memcpy(response.originationMsgId, msgId.c_str(), msgIdSize);
            memcpy(response.message, msgParts[3].c_str(), configSize);

            if (xQueueSend(this->handlerQueue, &response, pdTICKS_TO_MS(250)) == pdFALSE)
            {
                ESP_LOGE(TAG, "Failed to send message to handler queue");
                free(response.originationMsgId);
                free(response.message);
            }
        }
        else
        {
            astros_interface_response_t response = {
                .type = AstrOsInterfaceResponseType::SEND_CONFIG,
                .originationMsgId = (char *)malloc(msgIdSize),
                .peerMac = (char *)malloc(peerSize),
                .peerName = nullptr,
                .message = (char *)malloc(configSize)};

            memcpy(response.originationMsgId, msgId.c_str(), msgIdSize);
            memcpy(response.peerMac, msgParts[0].c_str(), peerSize);
            memcpy(response.message, msgParts[3].c_str(), configSize);

            if (xQueueSend(this->handlerQueue, &response, pdTICKS_TO_MS(250)) == pdFALSE)
            {
                ESP_LOGE(TAG, "Failed to send message to handler queue");
                free(response.originationMsgId);
                free(response.peerMac);
                free(response.message);
            }
        }
    }
}

void AstrOsSerialMsgHandler::sendBasicAckNakResponse(AstrOsSerialMessageType type, std::string msgId, std::string mac, std::string name, std::string payload)
{
    auto response = AstrOsSerialMessageService::getBasicAckNak(type, msgId, mac, name, payload);

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
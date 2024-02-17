
#include <AstrOsSerialMsgHandler.hpp>
#include <AstrOsServerResponseMsg.hpp>
#include <AstrOsMessaging.hpp>
#include <esp_log.h>
#include <string.h>

const char *TAG = "AstrOsSerialInterface";

AstrOsSerialMsgHandler AstrOs_SerialMsgHandler;

AstrOsSerialMsgHandler::AstrOsSerialMsgHandler() {}

AstrOsSerialMsgHandler::~AstrOsSerialMsgHandler() {}

void AstrOsSerialMsgHandler::Init(QueueHandle_t serverResponseQueue)
{
    this->serverResponseQueue = serverResponseQueue;
}

void AstrOsSerialMsgHandler::handleMessage(std::string message)
{

    astros_serial_msg_validation_t validation = AstrOsSerialMessageService::validateSerialMsg(message);

    if (!validation.valid)
    {
        ESP_LOGE(TAG, "Invalid message: %s", message.c_str());
        return;
    }

    switch (validation.type)
    {
    case AstrOsSerialMessageType::REGISTRATION_SYNC:
    {
        astros_server_response_t response = {
            .type = AstrOsServerResponseType::REGISTRATION_SYNC,
            .originationMsgId = (char *)malloc(validation.msgId.size() + 1),
            .message = nullptr};

        memcpy(response.originationMsgId, validation.msgId.c_str(), validation.msgId.size() + 1);

        if (xQueueSend(this->serverResponseQueue, &response, pdTICKS_TO_MS(250)) == pdFALSE)
        {
            ESP_LOGE(TAG, "Failed to send message to server response queue");
        }
        break;
    }
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

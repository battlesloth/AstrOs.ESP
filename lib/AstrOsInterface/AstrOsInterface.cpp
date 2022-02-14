
#include <AstrOsInterface.h>
#include <AstrOsUtility.h>

AstrOsInterface AstrOs;

AstrOsInterface::AstrOsInterface(){}

AstrOsInterface::~AstrOsInterface(){}

void AstrOsInterface::Init(QueueHandle_t animationQueue){
    AstrOsInterface::animationQueue = animationQueue;
}

void AstrOsInterface::handleMessage(char *data){

            queue_msg_t msg = {1, data};
            xQueueSend(animationQueue, &msg, 0);
}
//static int sendData(const char *logName, const char *data)
//{
//    const int len = strlen(data);
//    const int txBytes = uart_write_bytes(UART_PORT, data, len);
//    ESP_LOGI(logName, "Wrote %d bytes", txBytes);
//    return txBytes;
//}



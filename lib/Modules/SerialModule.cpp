#include <SerialModule.h>
#include <AnimationCommand.h>
#include <AstrOsUtility.h>

#include "esp_system.h"
#include "driver/uart.h"
#include <esp_log.h>
#include <string>
#include <string.h>

static const char *TAG = "SerialModule";

#define UART_PORT UART_NUM_1

static const int RX_BUF_SIZE = 1024;

SerialModule SerialMod;

SerialModule::SerialModule(){}

SerialModule::~SerialModule(){}

esp_err_t SerialModule::Init(int baud_rate, int rx_pin, int tx_pin){
    esp_err_t result;

    const uart_config_t uart_config = {
        .baud_rate = baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT};

    esp_err_t err = uart_param_config(UART_PORT, &uart_config);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    err = uart_set_pin(UART_PORT, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    return uart_driver_install(UART_PORT, RX_BUF_SIZE * 2, 0, 0, NULL, 0);

    return result;
}


void SerialModule::SendCommand(const char* cmd){
    ESP_LOGI(TAG, "Sending Command => %s", cmd); 
    SerialModule::SendData(cmd);
}


void SerialModule::SendData(const char* cmd)
{
    const int len = strlen(cmd);
    const int txBytes = uart_write_bytes(UART_PORT, cmd, len);
    ESP_LOGI(TAG, "Wrote %d bytes", txBytes);
}

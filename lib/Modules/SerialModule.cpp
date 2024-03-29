#include <SerialModule.h>
#include <AnimationCommand.h>
#include <AstrOsUtility.h>
#include <SoftwareSerial.h>

#include "esp_system.h"
#include "driver/uart.h"
#include <esp_log.h>
#include <string>
#include <string.h>

static const char *TAG = "SerialModule";
static pthread_mutex_t serialMutex;

static const int RX_BUF_SIZE = 1024;

SerialModule SerialMod;

SwSerial *softSerial;

SerialModule::SerialModule() {}

SerialModule::~SerialModule() {}

esp_err_t SerialModule::Init(serial_config_t cfig)
{
    esp_err_t result = ESP_OK;

    if (pthread_mutex_init(&serialMutex, NULL) != 0)
    {
        ESP_LOGE(TAG, "Failed to initialize the serial mutex");
    }

    rx[0] = cfig.rxPin1;
    rx[1] = cfig.rxPin2;
    rx[2] = cfig.rxPin3;

    tx[0] = cfig.txPin1;
    tx[1] = cfig.txPin2;
    tx[2] = cfig.txPin3;

    baud[0] = cfig.baudRate1;
    baud[1] = cfig.baudRate2;
    baud[2] = cfig.baudRate3;

    result = SerialModule::InstallSerial(UART_NUM_1, tx[0], rx[0], baud[0]);

    if (result != ESP_OK)
    {
        return result;
    }

    ESP_LOGI(TAG, "%d:%d:%d", tx[1], rx[1], baud[1]);

    result = SerialModule::InstallSerial(UART_NUM_2, tx[1], rx[1], baud[1]);

    softSerial = sw_new((gpio_num_t)tx[2], (gpio_num_t)rx[2], true, 512);
    sw_open(softSerial, 9600);

    return result;
}

esp_err_t SerialModule::InstallSerial(uart_port_t port, int tx, int rx, int baud)
{

    ESP_LOGI(TAG, "UART tx:%d,rx:%d", tx, rx);
    esp_err_t err = ESP_OK;

    const uart_config_t config = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT};

    err = uart_param_config(port, &config);

    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    err = uart_set_pin(port, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    uart_driver_install(port, RX_BUF_SIZE * 2, 0, 0, NULL, 0);
    logError(TAG, __FUNCTION__, __LINE__, err);

    return err;
}

void SerialModule::SendCommand(const char *cmd)
{
    ESP_LOGI(TAG, "Sending Command => %s", cmd);

    auto command = SerialCommand(cmd);

    SerialModule::SendData(command.serialChannel, command.GetValue().c_str());
}

void SerialModule::SendData(int ch, std::string msg)
{
    if (pthread_mutex_lock(&serialMutex) == 0)
    {
        uart_port_t port;
        esp_err_t err;

        switch (ch)
        {
        case 1:
            port = UART_NUM_1;
            break;
        case 2:
            port = UART_NUM_2;
            break;
        case 3:
            SerialModule::SoftSerialWrite(msg.c_str());
            pthread_mutex_unlock(&serialMutex);
            return;
        default:
            port = UART_NUM_1;
            break;
        }

        const int txBytes = uart_write_bytes(port, msg.c_str(), msg.length() + 1);
        ESP_LOGI(TAG, "Wrote %d bytes", txBytes);

        pthread_mutex_unlock(&serialMutex);
    }
}

void SerialModule::SoftSerialWrite(const char *msg)
{
    int len = 0;
    while (msg[len] != '\0')
    {
        sw_write(softSerial, msg[len]);
        len++;
    }
    sw_write(softSerial, '\0');
    ESP_LOGI(TAG, "Soft Serial Wrote %d characters", len);
}

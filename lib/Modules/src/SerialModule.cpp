#include <SerialModule.hpp>
#include <AnimationCommands.hpp>
#include <AstrOsUtility.h>
#include <AstrOsUtility_Esp.h>

#include <esp_system.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <string>
#include <string.h>

static const char *TAG = "SerialModule";
static SemaphoreHandle_t serialMutex;

static const int RX_BUF_SIZE = 1024;

SerialModule SerialMod;

SerialModule::SerialModule() {}

SerialModule::~SerialModule() {}

esp_err_t SerialModule::Init(serial_config_t cfig)
{
    esp_err_t result = ESP_OK;

    serialMutex = xSemaphoreCreateMutex();
    if (serialMutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialize the serial mutex for port %d", cfig.port);
        return ESP_FAIL;
    }

    this->port = cfig.port;
    this->rx = cfig.rxPin;
    this->tx = cfig.txPin;
    this->defaultBaudrate = cfig.defaultBaudRate;
    this->isMaster = cfig.isMaster;
    
    result = SerialModule::InstallSerial(port, tx, rx, defaultBaudrate);

    if (result != ESP_OK)
    {
        return result;
    }

    return result;
}

esp_err_t SerialModule::InstallSerial(uart_port_t port, int tx, int rx, int baud)
{
    esp_err_t err = ESP_OK;

    const uart_config_t config = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_DEFAULT,
    };

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

void SerialModule::SendCommand(uint8_t *cmd)
{
    ESP_LOGD(TAG, "Sending Command => %s", cmd);

    auto command = SerialCommand(std::string(reinterpret_cast<char *>(cmd)));

    SerialModule::SendData(command.baudRate, reinterpret_cast<const uint8_t *>(command.GetValue().c_str()), command.GetValue().size());
}

void SerialModule::SendBytes(int baud, uint8_t *data, size_t size)
{
    SerialModule::SendData(baud, reinterpret_cast<const uint8_t *>(data), size);
}

void SerialModule::SendData(int baud, const uint8_t *data, size_t size)
{
    bool sent = false;

    while (!sent)
    {
        if (xSemaphoreTake(serialMutex, 100 / portTICK_PERIOD_MS))
        {
            // don't change the baud rate if this is the master node
            if (!this->isMaster)
            {
                uart_set_baudrate(port, baud);
            }
            
            const int txBytes = uart_write_bytes(port, data, size);
            ESP_LOGD(TAG, "Wrote %d bytes", txBytes);
            sent = true;
            xSemaphoreGive(serialMutex);
        }
        else
        {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }
}


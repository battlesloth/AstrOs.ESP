#include <AnimationCommands.hpp>
#include <AstrOsUtility.h>
#include <AstrOsUtility_ESP.h>
#include <SerialModule.hpp>

#include <driver/uart.h>
#include <esp_log.h>
#include <esp_system.h>
#include <string.h>
#include <string>

static const char *TAG = "SerialModule";
static SemaphoreHandle_t serialMutex;

static const int RX_BUF_SIZE = 1024;
// A full 128-byte TX FIFO takes 133 ms at 9600 baud, the slowest rate the server offers.
static const int TX_DONE_WAIT_MS = 500;
// Rates a port may be set to. The server offers 9600-115200; the UART does 5 Mbaud.
static const int MIN_BAUD = 300;
static const int MAX_BAUD = 5000000;

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

    this->currentBaud = baud;

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

    SerialModule::SendData(command.baudRate, reinterpret_cast<const uint8_t *>(command.GetValue().c_str()),
                           command.GetValue().size());
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
            if (!this->isMaster && (baud < MIN_BAUD || baud > MAX_BAUD))
            {
                // The HAL accepts some bad rates (-1 from a malformed command, or
                // anything above its 80 MHz clock) as divider 0, which would stall
                // TX while this task holds the mutex both channels share. Send at
                // the current rate: a drop here could lose a panic's servo offs.
                ESP_LOGE(TAG, "Serial port %d - Invalid baudrate %d, keeping %d", this->port, baud, this->currentBaud);
            }
            // don't change the baud rate if this is the master node
            else if (!this->isMaster && this->currentBaud != baud)
            {
                // uart_write_bytes returns with the last message still in the TX
                // FIFO, and uart_set_baudrate doesn't wait for it: let it finish,
                // or it goes out at the new rate.
                auto err = uart_wait_tx_done(port, pdMS_TO_TICKS(TX_DONE_WAIT_MS));

                if (logError(TAG, __FUNCTION__, __LINE__, err))
                {
                    ESP_LOGW(TAG, "Serial port %d - TX not done after %d ms, changing baudrate to %d anyway",
                             this->port, TX_DONE_WAIT_MS, baud);
                }

                err = uart_set_baudrate(port, (uint32_t)baud);

                if (logError(TAG, __FUNCTION__, __LINE__, err))
                {
                    ESP_LOGE(TAG, "Serial port %d - Failed to set baudrate %d!", this->port, baud);
                }
                else
                {
                    this->currentBaud = baud;
                }
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

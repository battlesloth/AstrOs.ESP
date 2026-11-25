#include <AnimationCommands.hpp>
#include <AstrOsUtility.h>
#include <AstrOsUtility_ESP.h>
#include <SerialModule.hpp>

#include <algorithm>
#include <driver/uart.h>
#include <esp_log.h>
#include <esp_system.h>
#include <soc/soc_caps.h>
#include <string.h>
#include <string>

static const char *TAG = "SerialModule";
static SemaphoreHandle_t serialMutex;

static const int RX_BUF_SIZE = 1024;
// On top of the time a full TX FIFO takes to go out (txDoneWaitMs).
static const int TX_DONE_SLACK_MS = 100;
// Quiet time on the line before a speed change. A character at 9600, the slowest rate
// the server offers, takes about 1 ms.
static const int BAUD_CHANGE_GAP_MS = 10;
// Rates a port may be set to. The server offers 9600-115200; the UART does 5 Mbaud.
static const int MIN_BAUD = 300;
static const int MAX_BAUD = 5000000;

static bool isValidBaud(int baud)
{
    return baud >= MIN_BAUD && baud <= MAX_BAUD;
}

// The longest a wait for TX can normally take: a full FIFO at 10 bits a byte (8N1),
// 133 ms at 9600 and 4.3 s at MIN_BAUD. currentBaud is 0 until the port is set up.
static int txDoneWaitMs(int baud)
{
    return SOC_UART_FIFO_LEN * 10 * 1000 / std::max(baud, MIN_BAUD) + TX_DONE_SLACK_MS;
}

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

    if (!isValidBaud(command.baudRate))
    {
        ESP_LOGE(TAG, "Invalid baudrate %d in serial command => %s", command.baudRate, cmd);
    }

    SerialModule::SendData(command.baudRate, reinterpret_cast<const uint8_t *>(command.GetValue().c_str()),
                           command.GetValue().size());
}

void SerialModule::SendBytes(int baud, uint8_t *data, size_t size)
{
    SerialModule::SendData(baud, reinterpret_cast<const uint8_t *>(data), size);
}

void SerialModule::WaitTxDone()
{
    auto err = uart_wait_tx_done(this->port, pdMS_TO_TICKS(txDoneWaitMs(this->currentBaud)));

    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        ESP_LOGW(TAG, "Serial port %d - Waiting for TX to finish failed, going on anyway", this->port);
    }
}

void SerialModule::SendData(int baud, const uint8_t *data, size_t size)
{
    bool sent = false;

    if (!this->isMaster && !isValidBaud(baud))
    {
        // The HAL accepts some bad rates (-1 from a malformed command, or
        // anything above its 80 MHz clock) as divider 0, which would stall TX
        // while this task holds the mutex both channels share. Send at the
        // current rate: a drop here could lose a panic's servo offs.
        ESP_LOGE(TAG, "Serial port %d - Invalid baudrate %d, keeping %d", this->port, baud, this->currentBaud);
        baud = this->currentBaud;
    }

    while (!sent)
    {
        if (xSemaphoreTake(serialMutex, 100 / portTICK_PERIOD_MS))
        {
            // don't change the baud rate if this is the master node
            if (!this->isMaster && this->currentBaud != baud)
            {
                // uart_write_bytes returns with the last message still in the TX
                // FIFO, and uart_set_baudrate doesn't wait for it: let it finish,
                // or it goes out at the new rate.
                this->WaitTxDone();

                // Devices sharing the line hear each other's rate as noise, and need
                // it quiet to find the next start bit. Best effort (T-012).
                // One tick more: vTaskDelay(n) can end up to a tick short of n.
                vTaskDelay(pdMS_TO_TICKS(BAUD_CHANGE_GAP_MS) + 1);

                auto err = uart_set_baudrate(port, (uint32_t)baud);

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

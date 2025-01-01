#include <SerialModule.hpp>
#include <AnimationCommand.hpp>
#include <AstrOsUtility.h>
#include <AstrOsUtility_Esp.h>
// #include <SoftwareSerial.h>

#include <esp_system.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <string>
#include <string.h>

static const char *TAG = "SerialModule";
static SemaphoreHandle_t serialMutex;

static const int RX_BUF_SIZE = 1024;

SerialModule SerialMod;

// SwSerial *softSerial;

SerialModule::SerialModule() {}

SerialModule::~SerialModule() {}

esp_err_t SerialModule::Init(serial_config_t cfig)
{
    esp_err_t result = ESP_OK;

    serialMutex = xSemaphoreCreateMutex();
    if (serialMutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialize the serial mutex");
    }

    rx[0] = cfig.rxPin1;
    //rx[1] = cfig.rxPin2;
    //rx[2] = cfig.rxPin3;

    tx[0] = cfig.txPin1;
    //tx[1] = cfig.txPin2;
    //tx[2] = cfig.txPin3;

    baud[0] = cfig.baudRate1;
    //baud[1] = cfig.baudRate2;
    //baud[2] = cfig.baudRate3;

    result = SerialModule::InstallSerial(UART_NUM_1, tx[0], rx[0], baud[0]);

    if (result != ESP_OK)
    {
        return result;
    }

    //result = SerialModule::InstallSerial(UART_NUM_2, tx[1], rx[1], baud[1]);

    // softSerial = sw_new((gpio_num_t)tx[2], (gpio_num_t)rx[2], true, 512);
    // sw_open(softSerial, 9600);

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

    SerialModule::SendData(command.serialChannel, reinterpret_cast<const uint8_t *>(command.GetValue().c_str()), command.GetValue().size());
}

void SerialModule::SendBytes(int ch, uint8_t *data, size_t size)
{
    SerialModule::SendData(ch, reinterpret_cast<const uint8_t *>(data), size);
}

void SerialModule::SendData(int ch, const uint8_t *data, size_t size)
{
    bool sent = false;

    while (!sent)
    {
        if (xSemaphoreTake(serialMutex, 100 / portTICK_PERIOD_MS))
        {
            uart_port_t port = UART_NUM_1;

            // Maestro is using UART 2
            // SoftSerial isn't working
            /*
            switch (ch)
            {
            case 1:
                port = UART_NUM_1;
                break;
            case 2:
                port = UART_NUM_2;
                break;
            case 3:
                // SerialModule::SoftSerialWrite(msg.c_str());
                sent = true;
                xSemaphoreGive(serialMutex);
                return;
            default:
                port = UART_NUM_1;
                break;
            }
            */

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

/*void SerialModule::SoftSerialWrite(const char *msg)
{
    int len = 0;
    while (msg[len] != '\0')
    {
        sw_write(softSerial, msg[len]);
        len++;
    }
    sw_write(softSerial, '\0');
    ESP_LOGI(TAG, "Soft Serial Wrote %d characters", len);
}*/

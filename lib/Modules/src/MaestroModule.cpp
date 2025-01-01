#include "MaestroModule.hpp"

#include <AstrOsUtility.h>
#include <AstrOsStorageManager.hpp>
#include <AnimationCommand.hpp>

#include <esp_log.h>
#include <esp_system.h>
#include <driver/uart.h>
#include <AstrOsUtility_ESP.h>
#include <AstrOsServoUtils.hpp>
#include <AstrOsUtility.h>

static const char *TAG = "MaestroModule";
static const int RX_BUF_SIZE = 1024;

static SemaphoreHandle_t serialMutex;

MaestroModule MaestroMod;

servo_channel channels[24] = {};

MaestroModule::MaestroModule()
{
    serialMutex = xSemaphoreCreateMutex();
}

MaestroModule::~MaestroModule()
{
}

esp_err_t MaestroModule::Init(maestro_config_t config)
{
    ESP_LOGI(TAG, "Initializing Maestro Module");

    esp_err_t err = ESP_OK;

    this->config = config;

    err = this->InitSerial();

    if (err != ESP_OK)
    {
        return err;
    }

    this->LoadConfig();

    return err;
}

esp_err_t MaestroModule::InitSerial()
{
    esp_err_t err = ESP_OK;

    const uart_config_t cfg = {
        .baud_rate = config.baudRate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_DEFAULT,
    };

    err = uart_param_config(config.port, &cfg);

    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    err = uart_set_pin(config.port, config.txPin, config.rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    uart_driver_install(config.port, RX_BUF_SIZE * 2, 0, 0, NULL, 0);
    logError(TAG, __FUNCTION__, __LINE__, err);

    return err;
}

void MaestroModule::LoadConfig()
{
    this->loading = true;

    AstrOs_Storage.loadServoConfig(0, channels, 24);

    this->HomeServos();

    this->loading = false;
}

void MaestroModule::QueueCommand(uint8_t *cmd)
{
    ESP_LOGI(TAG, "Queueing servo command => %s", cmd);
    ServoCommand servoCmd = ServoCommand(std::string(reinterpret_cast<char *>(cmd)));

    if (servoCmd.channel > 23)
    {
        ESP_LOGE(TAG, "Invalid channel %d", servoCmd.channel);
        return;
    }

    int ch = servoCmd.channel;
    
    // set channel requested position to percentage of max - min taking into account inverted
    int requestPos = servoCmd.position;
    if (channels[ch].inverted)
    {
        requestPos = 100 - requestPos;
    }
    channels[ch].requestedPos = GetRelativeRequestedPosition(channels[ch].minPos, channels[ch].maxPos, servoCmd.position);
    
    ESP_LOGI(TAG, "Setting servo %d (min: %d, max: %d) to %d, speed: %d, accel: %d", ch, channels[ch].minPos, channels[ch].maxPos, channels[ch].requestedPos, servoCmd.speed, servoCmd.acceleration);

    channels[ch].currentPos = 0;
    channels[ch].speed = servoCmd.speed;
    channels[ch].acceleration = servoCmd.acceleration;
    channels[ch].on = true;

    this->setServoPosition(ch, channels[ch].requestedPos, channels[ch].lastPos, servoCmd.speed, servoCmd.acceleration);
}

void MaestroModule::SetServoPosition(uint8_t channel, int ms)
{
    if (this->loading)
    {
        return;
    }

    this->setServoPosition(channel, ms, -1, 0, 0);
}

void MaestroModule::Panic()
{
    ESP_LOGI(TAG, "Panic");

    uint8_t cmd[74] = {};
    cmd[0] = SET_MULTIPLE_SERVOS_COMMAND;
    cmd[1] = 24;

    for (size_t i = 0; i < 24; i++)
    {
        channels[i].on = false;
        cmd[2 + (i * 3)] = i;
    }

    bool sent = false;

    while (!sent)
    {
        if (xSemaphoreTake(serialMutex, 100 / portTICK_PERIOD_MS))
        {
            const int txBytes = uart_write_bytes(config.port, cmd, 74);

            sent = true;
            xSemaphoreGive(serialMutex);
        }
        else
        {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }
}

void MaestroModule::HomeServos()
{
    ESP_LOGI(TAG, "Homing Servos");

    for (size_t i = 0; i < 24; i++)
    {
        if (channels[i].set)
        {
            channels[i].requestedPos = channels[i].home;
            channels[i].currentPos = 0;
            channels[i].on = true;
            channels[i].speed = 0;
            channels[i].acceleration = 0;
            this->setServoPosition(i, channels[i].home, 0, 0, 0);
            channels[i].lastPos = channels[i].home;
        }
    }
}

/// @brief Since we can't really know where the servo is actually at
/// and we don't want to keep the servo on all the time, we need to
/// see if the servo could have moved it's entire range in the time
/// since the last command. If it could have, we need to turn it off.
/// @param msSinceLastCheck The time since the last check in milliseconds
void MaestroModule::CheckServos(int msSinceLastCheck)
{
    for (size_t i = 0; i < 24; i++)
    {
        if (channels[i].on)
        {
            // speed is (.25us/10ms) * n, where n is 0-255
            // 0 is no speed limit
            // An extended range servo is 500-2500us at 180 degrees
            // to give us some buffer, we will calculate 0 to 3000 us
            // if accel is less than speed, we will use accel to account for
            // slower start and stop
            double speed = channels[i].speed == 0 ? 255 : channels[i].speed;
            if (channels[i].acceleration < speed && channels[i].acceleration != 0)
            {
                speed = channels[i].acceleration;
            }

            channels[i].currentPos = channels[i].currentPos + (((.25 * speed) * (msSinceLastCheck/10))/ 4);

            if (channels[i].currentPos >= 3000)
            {
                ESP_LOGI(TAG, "Turning off servo %d", i);
                this->setServoOff(i);
                channels[i].on = false;
                channels[i].currentPos = 0;
            }
        }
    }
}

void MaestroModule::setServoPosition(uint8_t channel, int ms, int lastpos, int speed, int acceleration)
{
    bool sent = false;

    while (!sent)
    {
        if (xSemaphoreTake(serialMutex, 100 / portTICK_PERIOD_MS))
        {
            uint8_t cmd[4] = {};
            
            cmd[1] = channel;

            int txBytes = 0;

            // we need to send the last requested position
            // before we send speed/accel commands if the servo
            // was set to off as these commands will not work
            // if they happen before the servo is turned on
            if (lastpos != -1)
            {
                cmd[0] = SET_SERVO_COMMAND;
                cmd[2] = lastpos & 0x7F;
                cmd[3] = (lastpos >> 7) & 0x7F;

                txBytes += uart_write_bytes(config.port, cmd, 4);
            }
            
            cmd[0] = SET_SERVO_SPEED_COMMAND;
            cmd[2] = speed & 0x7F;
            cmd[3] = (speed >> 7) & 0x7F;

            txBytes += uart_write_bytes(config.port, cmd, 4);

            cmd[0] = SET_SERVO_ACCELERATION_COMMAND;
            cmd[2] = acceleration & 0x7F;
            cmd[3] = (acceleration >> 7) & 0x7F;

            txBytes += uart_write_bytes(config.port, cmd, 4);

            // .25us resolution
            int target = ms * 4;

            cmd[0] = SET_SERVO_COMMAND;
            cmd[2] = target & 0x7F;
            cmd[3] = (target >> 7) & 0x7F;

            txBytes += uart_write_bytes(config.port, cmd, 4);

            ESP_LOGD(TAG, "Wrote %d bytes to serial", txBytes);

            sent = true;
            xSemaphoreGive(serialMutex);
        }
        else
        {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }
}

void MaestroModule::setServoOff(uint8_t channel)
{
    bool sent = false;

    while (!sent)
    {
        if (xSemaphoreTake(serialMutex, 100 / portTICK_PERIOD_MS))
        {
            uint8_t cmd[4] = {};

            cmd[0] = SET_SERVO_COMMAND;
            cmd[1] = channel;

            const int txBytes = uart_write_bytes(config.port, cmd, 4);

            sent = true;
            xSemaphoreGive(serialMutex);
        }
        else
        {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }
}

void MaestroModule::getError()
{
    bool sent = false;

    while (!sent)
    {
        if (xSemaphoreTake(serialMutex, 100 / portTICK_PERIOD_MS))
        {
            uint8_t cmd[1] = {};

            cmd[0] = GET_ERROR_COMMAND;

            const int txBytes = uart_write_bytes(config.port, cmd, 1);

            sent = true;
            xSemaphoreGive(serialMutex);
        }
        else
        {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }
}

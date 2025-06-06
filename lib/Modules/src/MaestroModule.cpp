#include "MaestroModule.hpp"

#include <AstrOsUtility.h>
#include <AstrOsStorageManager.hpp>
#include <AnimationCommands.hpp>

#include <esp_log.h>
#include <esp_system.h>
#include <driver/uart.h>
#include <AstrOsUtility_ESP.h>
#include <AstrOsServoUtils.hpp>
#include <AstrOsUtility.h>
#include <SerialModule.hpp>
#include <string.h>

static const char *TAG = "MaestroModule";
static const int RX_BUF_SIZE = 1024;

servo_channel channels[24] = {};

MaestroModule::MaestroModule(QueueHandle_t serialQueue, int idx, int baudRate)
{

    this->mutex = xSemaphoreCreateMutex();
    if (this->mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        return;
    }

    this->idx = idx;
    this->baudRate = baudRate;
    this->serialQueue = serialQueue;
    this->loading = false;
}

MaestroModule::~MaestroModule()
{
}

void MaestroModule::UpdateConfig(QueueHandle_t serialQueue, int baudRate)
{
    bool configChanged = false;

    while (!configChanged)
    {
        // don't update config while we are sending commands to servos
        if (xSemaphoreTake(this->mutex, 100 / portTICK_PERIOD_MS))
        {

            ESP_LOGI(TAG, "Updating Maestro module %d config, old buad: %d, new baud %d", this->idx, this->baudRate, baudRate);

            this->serialQueue = serialQueue;
            this->baudRate = baudRate;

            configChanged = true;
            xSemaphoreGive(this->mutex);
        }
        else
        {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }
}

void MaestroModule::LoadConfig()
{
    this->loading = true;

    ESP_LOGI(TAG, "Loading Maestro servos for module %d", this->idx);

    AstrOs_Storage.loadMaestroServos(this->idx, channels, 24);

    this->HomeServos();

    this->loading = false;
}

void MaestroModule::QueueCommand(uint8_t *cmd)
{
    ESP_LOGI(TAG, "Queueing servo command => %s", cmd);
    MaestroCommand servoCmd = MaestroCommand(std::string(reinterpret_cast<char *>(cmd)));

    if (servoCmd.channel > 23 || servoCmd.channel < 0)
    {
        ESP_LOGE(TAG, "Invalid channel %d", servoCmd.channel);
        return;
    }

    int ch = servoCmd.channel;

    // set channel requested position to percentage of max - min taking into account inverted
    int requestPos = servoCmd.position;

    // if it's not a servo it's an on/off GPIO
    if (!channels[ch].isServo)
    {
        channels[ch].requestedPos = requestPos >= 1500 ? 2500 : 500;
    }
    else if (requestPos < 0)
    {
        channels[ch].requestedPos = channels[ch].home;
    }
    else
    {
        if (channels[ch].inverted)
        {
            requestPos = 100 - requestPos;
        }
        channels[ch].requestedPos = GetRelativeRequestedPosition(channels[ch].minPos, channels[ch].maxPos, requestPos);
    }

    ESP_LOGI(TAG, "Setting servo %d (min: %d, max: %d) to %d, cmd: %d. speed: %d. accel: %d. inverted: %d", ch, channels[ch].minPos,
             channels[ch].maxPos, channels[ch].requestedPos, servoCmd.position, servoCmd.speed, servoCmd.acceleration, channels[ch].inverted);

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

    this->sendQueueMsg(cmd, 74);
}

void MaestroModule::HomeServos()
{
    ESP_LOGI(TAG, "Homing Servos");

    for (size_t i = 0; i < 24; i++)
    {
        if (channels[i].enabled)
        {
            if (!channels[i].isServo)
            {
                if (channels[i].inverted){
                    this->setServoPosition(i, 2500, 0, 0, 0);
                }
                else {
                    this->setServoPosition(i, 500, 0, 0, 0);
                }
            }
            else
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
        if (!channels[i].isServo)
        {
            continue;
        }
        if (channels[i].on)
        {
            // speed is (.25us/10ms) * n, where n is 0-255
            // 0 is no speed limit
            // An extended range servo is 500-2500us at 180 degrees
            // to give us some buffer, we will calculate 0 to 3000 us
            // if accel is less than speed, we will use accel to account for
            // slower start and stop
            // UPDATE: increase the timeout to accomidate linear acuators Doug is using
            // being slow as fuck to react to changes
            double speed = channels[i].speed == 0 ? 255 : channels[i].speed;
            if (channels[i].acceleration < speed && channels[i].acceleration != 0)
            {
                speed = channels[i].acceleration;
            }

            channels[i].currentPos = channels[i].currentPos + (((.25 * speed) * (msSinceLastCheck / 100)) / 4);

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
    uint8_t cmd[4] = {};

    cmd[1] = channel;

    // we need to send the last requested position
    // before we send speed/accel commands if the servo
    // was set to off as these commands will not work
    // if they happen before the servo is turned on
    if (lastpos != -1)
    {
        cmd[0] = SET_SERVO_COMMAND;
        cmd[2] = lastpos & 0x7F;
        cmd[3] = (lastpos >> 7) & 0x7F;

        this->sendQueueMsg(cmd, 4);
    }

    cmd[0] = SET_SERVO_SPEED_COMMAND;
    cmd[2] = speed & 0x7F;
    cmd[3] = (speed >> 7) & 0x7F;

    this->sendQueueMsg(cmd, 4);

    cmd[0] = SET_SERVO_ACCELERATION_COMMAND;
    cmd[2] = acceleration & 0x7F;
    cmd[3] = (acceleration >> 7) & 0x7F;

    this->sendQueueMsg(cmd, 4);

    // .25us resolution
    int target = ms * 4;

    cmd[0] = SET_SERVO_COMMAND;
    cmd[2] = target & 0x7F;
    cmd[3] = (target >> 7) & 0x7F;

    this->sendQueueMsg(cmd, 4);

    ESP_LOGD(TAG, "command sent to channel: %d", channel);
}

void MaestroModule::setServoOff(uint8_t channel)
{
    uint8_t cmd[2] = {};

    cmd[0] = SET_SERVO_COMMAND;
    cmd[1] = channel;

    this->sendQueueMsg(cmd, 2);
}

void MaestroModule::getError()
{
    uint8_t cmd[1] = {};

    cmd[0] = GET_ERROR_COMMAND;

    this->sendQueueMsg(cmd, 1);
}

void MaestroModule::sendQueueMsg(uint8_t cmd[], size_t size)
{
    queue_serial_msg_t msg;

    msg.message_id = 1;
    msg.baudrate = this->baudRate;
    msg.data = (uint8_t *)malloc(size);
    memcpy(msg.data, cmd, size);
    msg.dataSize = size;

    bool sent = false;

    while (!sent)
    {
        if (xSemaphoreTake(this->mutex, 100 / portTICK_PERIOD_MS))
        {
            if (xQueueSend(this->serialQueue, &msg, pdMS_TO_TICKS(500)) != pdTRUE)
            {
                ESP_LOGW(TAG, "Send serial queue fail");
                free(msg.data);
            }
            sent = true;
            xSemaphoreGive(this->mutex);
        }
        else
        {
            vTaskDelay(10 / portTICK_PERIOD_MS); // wait a bit before retrying
        }
    }
}

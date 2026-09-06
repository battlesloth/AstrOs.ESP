#include "MaestroModule.hpp"

#include <AnimationCommands.hpp>
#include <AstrOsStorageManager.hpp>
#include <AstrOsUtility.h>

#include <AstrOsServoUtils.hpp>
#include <AstrOsUtility.h>
#include <AstrOsUtility_ESP.h>
#include <SerialModule.hpp>
#include <driver/uart.h>
#include <esp_log.h>
#include <esp_system.h>
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

MaestroModule::~MaestroModule() {}

void MaestroModule::UpdateConfig(QueueHandle_t serialQueue, int baudRate)
{
    bool configChanged = false;

    while (!configChanged)
    {
        // don't update config while we are sending commands to servos
        if (xSemaphoreTake(this->mutex, 100 / portTICK_PERIOD_MS))
        {

            ESP_LOGI(TAG, "Updating Maestro module %d config, old buad: %d, new baud %d", this->idx, this->baudRate,
                     baudRate);

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

    ESP_LOGI(TAG, "Setting servo %d (min: %d, max: %d) to %d, cmd: %d. speed: %d. accel: %d. inverted: %d", ch,
             channels[ch].minPos, channels[ch].maxPos, channels[ch].requestedPos, servoCmd.position, servoCmd.speed,
             servoCmd.acceleration, channels[ch].inverted);

    channels[ch].currentPos = 0;
    channels[ch].speed = servoCmd.speed;
    channels[ch].acceleration = servoCmd.acceleration;
    channels[ch].on = true;

    this->setServoPosition(ch, channels[ch].requestedPos, channels[ch].lastPos, servoCmd.speed, servoCmd.acceleration);
}

void MaestroModule::SetServoPosition(int channel, int ms)
{
    if (this->loading)
    {
        return;
    }

    // Validate as int: the caller parses the channel from text, and narrowing
    // to uint8_t before this check would wrap 256 to 0 and -1 to 255.
    if (channel < 0 || channel > 23)
    {
        ESP_LOGE(TAG, "Invalid channel %d", channel);
        return;
    }

    // Direct (slider) moves arrive as a stream at full speed. Arm release
    // tracking on every one, exactly as QueueCommand does: each re-arm just
    // resets the clock, so CheckServos turns the servo off 20 s (the floor)
    // after the *last* message and never mid-drag. Deliberately no INFO log
    // here -- it would spam the monitor at drag rate.
    channels[channel].currentPos = 0;
    channels[channel].speed = 0;
    channels[channel].acceleration = 0;
    channels[channel].on = true;

    this->setServoPosition(static_cast<uint8_t>(channel), ms, -1, 0, 0);
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
                if (channels[i].inverted)
                {
                    this->setServoPosition(i, 2500, 0, 0, 0);
                }
                else
                {
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
/// and we don't want to keep the servo on all the time, we accumulate
/// elapsed time per channel and turn the servo off once it has had
/// enough time to cover its entire range (worst case, with slack).
/// While a move is active, currentPos serves as the elapsed-ms
/// accumulator; the deadline comes from ServoReleaseDeadlineMs
/// (lib_native/AstrOsUtility/AstrOsServoUtils.hpp): the physical
/// worst case for the channel's speed/accel, floored at 20 s.
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
            channels[i].currentPos += msSinceLastCheck;

            if (channels[i].currentPos >= ServoReleaseDeadlineMs(channels[i].speed, channels[i].acceleration))
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
    uint8_t cmd[4] = {};

    cmd[0] = SET_SERVO_COMMAND;
    cmd[1] = channel;
    cmd[2] = 0x00;
    cmd[3] = 0x00;

    this->sendQueueMsg(cmd, 4);
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

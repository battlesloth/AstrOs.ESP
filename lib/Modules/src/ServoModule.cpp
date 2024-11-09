#include "ServoModule.hpp"
#include <AnimationCommand.hpp>
#include <Pca9685.hpp>
#include <AstrOsUtility.h>
#include <AstrOsStorageManager.hpp>

#include <esp_system.h>
#include <string>
#include <string.h>
#include <math.h>

#include <esp_log.h>
#include <pthread.h>
#include <AstrOsServoUtils.hpp>

static const char *TAG = "ServoModule";

Pca9685 pcaBoard0;
Pca9685 pcaBoard1;

uint16_t board0ServoFreq;
uint16_t board1ServoFreq;

SemaphoreHandle_t channelsMutex;

servo_channel channels0[16] = {};
servo_channel channels1[16] = {};

ServoModule ServoMod;

ServoModule::ServoModule()
{
}

ServoModule::~ServoModule()
{
}

esp_err_t ServoModule::Init(I2cMaster i2cMaster, servo_module_config_t config)
{
    esp_err_t result = ESP_OK;

    this->board0Freq = config.board0Freq;
    this->board0Slop = config.board0Slop;
    this->board1Freq = config.board1Freq;
    this->board1Slop = config.board1Slop;

    channelsMutex = xSemaphoreCreateMutex();

    if (channelsMutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialize the channels mutex");
        return ESP_FAIL;
    }

    // we need the actual frequency in Hz as a long for calculating steps since the
    // PCA9685 oscillator is not very accurate, but we round when we give it to the
    // board because the PCA9685 prescaler is an integer value
    result = pcaBoard0.Init(i2cMaster, config.board0Addr, std::round(board0Freq), board0Slop);

    if (result != ESP_OK)
    {
        return result;
    }

    result = pcaBoard1.Init(i2cMaster, config.board1Addr, std::round(board1Freq), board1Slop);
    if (result != ESP_OK)
    {
        return result;
    }

    CalculateStepMap(board0Freq, board0StepMap, SERVO_STEPS);
    CalculateStepMap(board1Freq, board1StepMap, SERVO_STEPS);

    this->LoadServoConfig();

    return result;
}

void ServoModule::LoadServoConfig()
{
    this->loading = true;
    ESP_LOGI(TAG, "Loading servo config");

    bool servosSet = false;

    double bord0Freq_us = 1000000.0 / this->board0Freq;
    auto board0MinStep = this->board0StepMap[0];
    auto board0MaxStep = this->board0StepMap[SERVO_STEPS - 1];

    double bord1Freq_us = 1000000.0 / this->board1Freq;
    auto board1MinStep = this->board1StepMap[0];
    auto board1MaxStep = this->board1StepMap[SERVO_STEPS - 1];

    while (!servosSet)
    {
        if (xSemaphoreTake(channelsMutex, 100 / portTICK_PERIOD_MS))
        {
            AstrOs_Storage.loadServoConfig(0, channels0, 16);
            AstrOs_Storage.loadServoConfig(1, channels1, 16);

            for (size_t i = 0; i < 16; i++)
            {
                // Board 0
                this->setChannelConfig(0, &channels0[i], board0MinStep, board0MaxStep, bord0Freq_us);

                // Board 1
                this->setChannelConfig(1, &channels1[i], board1MinStep, board1MaxStep, bord1Freq_us);
            }
            servosSet = true;
            xSemaphoreGive(channelsMutex);
        }
        else
        {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }

    ServoModule::ZeroServos();

    this->loading = false;
}

void ServoModule::QueueCommand(uint8_t *cmd)
{
    ESP_LOGI(TAG, "Queueing servo command => %s", cmd);
    ServoCommand servoCmd = ServoCommand(std::string(reinterpret_cast<char *>(cmd)));

    if (servoCmd.position == 666)
    {
        // this is a test command to set the channel
        // pulse command directly between 500 and 2500 degrees
        // in 1/3 degree steps (540)
        if (servoCmd.channel < 16)
        {
            auto pos = MicroSecondsToMapPosition(
                servoCmd.speed,
                (double)(1000000.0 / this->board0Freq),
                this->board0StepMap[0],
                this->board0StepMap[SERVO_STEPS - 1],
                SERVO_STEPS);

            pcaBoard0.SetPwm(servoCmd.channel, 0, pos);
        }
        else if (servoCmd.channel < 32)
        {
            auto pos = MicroSecondsToMapPosition(
                servoCmd.speed,
                (double)(1000000.0 / this->board0Freq),
                this->board1StepMap[0],
                this->board1StepMap[SERVO_STEPS - 1],
                SERVO_STEPS);

            pcaBoard1.SetPwm(servoCmd.channel - 16, 0, pos);
        }
    }
    else if (servoCmd.channel < 16)
    {
        ServoModule::setCommandByBoard(channels0, &servoCmd);
    }
    else if (servoCmd.channel < 32)
    {
        servoCmd.channel = servoCmd.channel - 16;
        ServoModule::setCommandByBoard(channels1, &servoCmd);
    }
}

void ServoModule::Panic()
{
    bool panicSet = false;

    while (!panicSet)
    {
        if (xSemaphoreTake(channelsMutex, 100 / portTICK_PERIOD_MS))
        {
            for (size_t i = 0; i < 16; i++)
            {
                channels0[i].requestedPos = channels0[i].currentPos;
                channels1[i].requestedPos = channels1[i].currentPos;
            }
            panicSet = true;
            xSemaphoreGive(channelsMutex);
        }
        else
        {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }
}

void ServoModule::ZeroServos()
{

    bool zeroSet = false;

    while (!zeroSet)
    {
        if (xSemaphoreTake(channelsMutex, 100 / portTICK_PERIOD_MS))
        {
            for (size_t i = 0; i < 16; i++)
            {
                this->zeroServo(&channels0[i]);
                this->zeroServo(&channels1[i]);
            }
            zeroSet = true;
            xSemaphoreGive(channelsMutex);
        }
        else
        {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }
}

/// @brief Call this method every n-ms depending on steps to move the servos.
// At speed 1, 250 ms, and 720 steps, this will be 1 degree a second, hopefully. CPU load
// could impact this.
void ServoModule::MoveServos()
{
    if (this->loading)
    {
        return;
    }

    bool moveSet = false;

    while (!moveSet)
    {
        if (xSemaphoreTake(channelsMutex, 100 / portTICK_PERIOD_MS))
        {
            for (size_t i = 0; i < 16; i++)
            {
                ServoModule::moveServoByBoard(&pcaBoard0, &channels0[i], i, board0StepMap);
                ServoModule::moveServoByBoard(&pcaBoard1, &channels1[i], i, board1StepMap);
            }
            moveSet = true;
            xSemaphoreGive(channelsMutex);
        }
        else
        {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }
}

void ServoModule::SetServoPosition(uint8_t board, uint8_t channel, int ms)
{
    if (board == 0)
    {
        if (channel < 16)
        {
            this->setServoToPosition(&pcaBoard0, &channels0[channel], board0StepMap, ms);
        }
    }
    else if (board == 1)
    {
        if (channel < 16)
        {
            this->setServoToPosition(&pcaBoard1, &channels1[channel], board1StepMap, ms);
        }
    }
}

//*********************************************************************************
// Private methods
//*********************************************************************************

void ServoModule::setChannelConfig(int boardId, servo_channel *channel, int minStep, int maxStep, double freqMs)
{
    auto min = MicroSecondsToMapPosition(channel->minPos, freqMs, minStep, maxStep, SERVO_STEPS);

    auto max = MicroSecondsToMapPosition(channel->maxPos, freqMs, minStep, maxStep, SERVO_STEPS);

    ESP_LOGI(TAG, "Min: %d, Max: %d", min, max);
    // this should never happend, but we can swap them
    // if it does
    if (min > max)
    {
        auto tmp = min;
        min = max;
        max = tmp;
    }

    // again, this should never happen, but we can clamp
    // to be certain
    channel->minPos = std::clamp(min, 0, SERVO_STEPS - 1);
    channel->maxPos = std::clamp(max, 0, SERVO_STEPS - 1);

    channel->requestedPos = channel->inverted ? channel->maxPos : channel->minPos;

    channel->on = false;

    ESP_LOGI(TAG, "Servo Config=> bd: %d ch: %d, min %d, max %d, set %d, inverted %d",
             boardId, channel->id, channel->minPos, channel->maxPos, channel->set, channel->inverted);
}

void ServoModule::zeroServo(servo_channel *servo)
{
    // if servo is inverted, it's 0 position is the max position
    servo->requestedPos = servo->inverted ? servo->maxPos : servo->minPos;
    servo->currentPos = GetRelativeRequestedPosition(servo->minPos, servo->maxPos, 20);
    servo->speed = SERVO_ZERO_SPEED;
    // only zero if the servo is set
    servo->on = servo->set;
}

void ServoModule::setCommandByBoard(servo_channel *servos, ServoCommand *cmd)
{

    bool commandSet = false;

    while (!commandSet)
    {
        if (xSemaphoreTake(channelsMutex, 100 / portTICK_PERIOD_MS))
        {
            int requestPos = cmd->position;

            if (servos[cmd->channel].inverted)
            {
                requestPos = 100 - requestPos;
            }

            servos[cmd->channel].requestedPos = GetRelativeRequestedPosition(
                servos[cmd->channel].minPos,
                servos[cmd->channel].maxPos,
                requestPos);
            servos[cmd->channel].speed = cmd->speed;
            servos[cmd->channel].on = true;
            commandSet = true;
            xSemaphoreGive(channelsMutex);
        }
        else
        {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }
}

void ServoModule::moveServoByBoard(Pca9685 *board, servo_channel *servo, int idx, uint16_t *stepMap)
{

    // if the servo is not not, return
    if (!servo->on)
    {
        return;
    }

    ESP_LOGI(TAG, "SERVO: %d, on: %d, set: %d",
             servo->id, servo->on, servo->set);

    // if the servo is not set, turn it off
    if (!servo->set && servo->on)
    {
        board->SetPwm(idx, 0, 4096);
        servo->on = false;
        ESP_LOGI(TAG, "Servo is not set. Setting servo channel %d on board %d off", idx, board->GetAddress());
        return;
    }

    // if the servo is at the requested position, turn it off
    // NOTE: may need to add a delay count here to keep the servo on to ensure
    // it reaches the requested position
    if (servo->currentPos == servo->requestedPos)
    {
        if (servo->on)
        {
            board->SetPwm(idx, 0, 4096);
            servo->on = false;
            ESP_LOGI(TAG, "Setting servo channel %d on board %d off", idx, board->GetAddress());
        }
    }

    // if the servo less than the requested position, move it up
    else if (servo->currentPos < servo->requestedPos)
    {
        servo->currentPos += servo->speed;

        if (servo->currentPos > servo->requestedPos)
        {
            servo->currentPos = servo->requestedPos;
        }
    }
    // if the servo is greater than the requested position, move it down
    else
    {
        servo->currentPos -= servo->speed;

        if (servo->currentPos < servo->requestedPos)
        {
            servo->currentPos = servo->requestedPos;
        }
    }

    auto pwmStep = stepMap[servo->currentPos];

    board->SetPwm(idx, 0, pwmStep);
}

void ServoModule::setServoToPosition(Pca9685 *board, servo_channel *channel, uint16_t *stepMap, int ms)
{
    auto pos = MicroSecondsToMapPosition(
        ms,
        (double)(1000000.0 / board->GetFrequency()),
        stepMap[0],
        stepMap[SERVO_STEPS - 1],
        SERVO_STEPS);

    board->SetPwm(channel->id, 0, board0StepMap[pos]);
    channel->currentPos = pos;
}

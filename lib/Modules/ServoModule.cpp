#include "ServoModule.h"
#include "AnimationCommand.h"
#include "Pca9685.h"
#include "AstrOsUtility.h"
#include "StorageManager.h"

#include "esp_system.h"
#include <string>
#include <string.h>
#include <math.h>

#include <esp_log.h>

#include <pthread.h>

#define ADDR PCA9685_ADDR_BASE
#ifndef APP_CPU_NUM
#define APP_CPU_NUM PRO_CPU_NUM
#endif

static const char *TAG = "ServoModule";

Pca9685 pcaBoard0;
Pca9685 pcaBoard1;

uint16_t SERVO_FREQ = 50;

static pthread_mutex_t channelsMutex;

servo_channel channels0[16] = {};
servo_channel channels1[16] = {};

ServoModule ServoMod;

ServoModule::ServoModule(/* args */)
{
}

ServoModule::~ServoModule()
{
}

esp_err_t ServoModule::Init()
{
    esp_err_t result = ESP_OK;

    if (pthread_mutex_init(&channelsMutex, NULL) != 0)
    {
        ESP_LOGE(TAG, "Failed to initialize the channels mutex");
    }

    ServoModule::LoadServoConfig();

    result = pcaBoard0.Init(0x40, SERVO_FREQ);

    if (result != ESP_OK)
    {
        return result;
    }

    result = pcaBoard1.Init(0x41, SERVO_FREQ);

    if (result != ESP_OK)
    {
        return result;
    }

    return result;
}

void ServoModule::LoadServoConfig()
{
    ESP_LOGI(TAG, "Loading servo config");

    if (pthread_mutex_lock(&channelsMutex) == 0)
    {

        Storage.loadServoConfig(0, channels0, 16);
        Storage.loadServoConfig(1, channels1, 16);

        for (size_t i = 0; i < 16; i++)
        {
            // Board 0

            // convert from micorseconds to PWM
            // based on 500 => 120, 2500 => 440
            // which works for the servos I tested with
            channels0[i].minPos = (int)((((4 * channels0[i].minPos) / 25) + 40) + 0.5);
            channels0[i].maxPos = (int)((((4 * channels0[i].maxPos) / 25) + 40) + 0.5);

            // zero on start up
            channels0[i].currentPos = channels0[i].minPos + 10;
            channels0[i].requestedPos = channels0[1].minPos;

            int x = (channels0[i].maxPos - channels0[i].minPos) / 100;
            channels0[i].moveFactor = (int)(x + 0.5);

            ESP_LOGI(TAG, "Servo Config=> bd: 0 ch: %d, min %d, max %d, set %d", channels0[i].id, channels0[i].minPos, channels0[i].maxPos, channels0[i].set);

            // Board 1

            // convert from micorseconds to PWM
            // based on 500 => 120, 2500 => 440
            // which works for the servos I tested with
            channels1[i].minPos = (int)((((4 * channels1[i].minPos) / 25) + 40) + 0.5);
            channels1[i].maxPos = (int)((((4 * channels1[i].maxPos) / 25) + 40) + 0.5);

            // zero on start up
            channels1[i].currentPos = channels1[i].minPos + 10;
            channels1[i].requestedPos = channels1[1].minPos;

            int y = (channels1[i].maxPos - channels1[i].minPos) / 100;
            channels1[i].moveFactor = (int)(y + 0.5);

            ESP_LOGI(TAG, "Servo Config=> bd: 1 ch: %d, min %d, max %d, set %d", channels1[i].id, channels1[i].minPos, channels1[i].maxPos, channels1[i].set);
        }

        pthread_mutex_unlock(&channelsMutex);
    }
}

void ServoModule::QueueCommand(const char *cmd)
{
    ESP_LOGI(TAG, "Queueing servo command => %s", cmd);
    ServoCommand servoCmd = ServoCommand(cmd);

    if (servoCmd.channel < 16)
    {
        ServoModule::SetCommandByBoard(channels0, &servoCmd);
    }
    else if (servoCmd.channel < 32)
    {
        servoCmd.channel = servoCmd.channel - 16;
        ServoModule::SetCommandByBoard(channels1, &servoCmd);
    }
}

void ServoModule::SetCommandByBoard(servo_channel *servos, ServoCommand *cmd)
{

    if (pthread_mutex_lock(&channelsMutex) == 0)
    {
        servos[cmd->channel].speed = cmd->speed;

        if (cmd->position <= 0)
        {
            servos[cmd->channel].requestedPos = servos[cmd->channel].minPos;
            pthread_mutex_unlock(&channelsMutex);
            return;
        }

        int move = servos[cmd->channel].moveFactor * cmd->position;

        servos[cmd->channel].requestedPos = servos[cmd->channel].minPos + move;

        if (servos[cmd->channel].requestedPos > servos[cmd->channel].maxPos || cmd->position >= 100)
        {
            servos[cmd->channel].requestedPos = servos[cmd->channel].maxPos;
        }

        pthread_mutex_unlock(&channelsMutex);
    }
}

void ServoModule::Panic()
{
    if (pthread_mutex_lock(&channelsMutex) == 0)
    {
        for (size_t i = 0; i < 16; i++)
        {
            channels0[i].requestedPos = channels0[i].currentPos;
            channels1[i].requestedPos = channels1[i].currentPos;
        }
        pthread_mutex_unlock(&channelsMutex);
    }
}

void ServoModule::ZeroServos()
{
    if (pthread_mutex_lock(&channelsMutex) == 0)
    {
        for (size_t i = 0; i < 16; i++)
        {
            channels0[i].requestedPos = channels0[i].minPos;
            channels1[i].requestedPos = channels1[i].minPos;
        }
        pthread_mutex_unlock(&channelsMutex);
    }
}

void ServoModule::MoveServos()
{
    if (pthread_mutex_lock(&channelsMutex) == 0)
    {
        for (size_t i = 0; i < 16; i++)
        {
            ServoModule::MoveServoByBoard(&pcaBoard0, channels0, i);
            ServoModule::MoveServoByBoard(&pcaBoard1, channels1, i);
        }

        pthread_mutex_unlock(&channelsMutex);
    }
}

void ServoModule::MoveServoByBoard(Pca9685 *board, servo_channel *servos, int idx)
{
    // only attempt to move servos that have a min/max
    if (!servos[idx].set)
    {
        return;
    }

    if (servos[idx].currentPos == servos[idx].requestedPos)
    {
        return;
    }
    else if (servos[idx].currentPos < servos[idx].requestedPos)
    {
        servos[idx].currentPos += servos[idx].speed;

        if (servos[idx].currentPos > servos[idx].requestedPos)
        {
            servos[idx].currentPos = servos[idx].requestedPos;
        }
        board->setPwm(idx, 0, servos[idx].currentPos);
    }
    else
    {

        servos[idx].currentPos -= servos[idx].speed;

        if (servos[idx].currentPos < servos[idx].requestedPos)
        {
            servos[idx].currentPos = servos[idx].requestedPos;
        }
        board->setPwm(idx, 0, servos[idx].currentPos);
    }
}
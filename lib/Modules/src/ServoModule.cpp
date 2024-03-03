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

// #define ADDR PCA9685_ADDR_BASE
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

esp_err_t ServoModule::Init(uint8_t board0addr, u_int8_t board1addr)
{
    esp_err_t result = ESP_OK;

    if (pthread_mutex_init(&channelsMutex, NULL) != 0)
    {
        ESP_LOGE(TAG, "Failed to initialize the channels mutex");
    }

    result = pcaBoard0.Init(board0addr, SERVO_FREQ);

    if (result != ESP_OK)
    {
        return result;
    }

    result = pcaBoard1.Init(board1addr, SERVO_FREQ);

    if (result != ESP_OK)
    {
        return result;
    }
    ServoModule::LoadServoConfig();
    // ServoModule::ZeroServos();

    return result;
}

void ServoModule::LoadServoConfig()
{
    ESP_LOGI(TAG, "Loading servo config");

    if (pthread_mutex_lock(&channelsMutex) == 0)
    {

        AstrOs_Storage.loadServoConfig(0, channels0, 16);
        AstrOs_Storage.loadServoConfig(1, channels1, 16);

        for (size_t i = 0; i < 16; i++)
        {
            // Board 0

            // convert from micorseconds to PWM
            // based on 500 => 100, 2500 => 510 as 20ms
            // which works for the servos I tested with

            auto min = (int)((channels0[i].minPos * 0.205) - 2.5);
            auto max = (int)((channels0[i].maxPos * 0.205) - 2.5);

            channels0[i].minPos = min < 0 ? 0 : min;
            channels0[i].maxPos = max < 0 ? 0 : max;

            // zero on start up
            if (channels0[i].inverted)
            {
                channels0[i].requestedPos = channels0[1].maxPos;
            }
            else
            {
                channels0[i].requestedPos = channels0[1].minPos;
            }

            int x = (channels0[i].maxPos - channels0[i].minPos) / 100;
            channels0[i].moveFactor = (int)(x + 0.5);
            channels0[i].on = false;

            ESP_LOGI(TAG, "Servo Config=> bd: 0 ch: %d, min %d, max %d, set %d, inverted %d",
                     channels0[i].id, channels0[i].minPos, channels0[i].maxPos, channels0[i].set, channels0[i].inverted);

            // Board 1

            // convert from micorseconds to PWM
            // based on 500 => 105, 2500 => 505 as 20ms
            // which works for the servos I tested with
            min = (int)((channels0[i].minPos * 0.205) - 2.5);
            max = (int)((channels0[i].maxPos * 0.205) - 2.5);
            channels1[i].minPos = min < 0 ? 0 : min;
            channels1[i].maxPos = max < 0 ? 0 : max;

            // zero on start up
            if (channels1[i].inverted)
            {
                channels1[i].requestedPos = channels1[1].maxPos;
            }
            else
            {
                channels1[i].requestedPos = channels1[i].minPos;
            }

            int y = (channels1[i].maxPos - channels1[i].minPos) / 100;
            channels1[i].moveFactor = (int)(y + 0.5);
            channels1[i].on = false;

            ESP_LOGI(TAG, "Servo Config=> bd: 1 ch: %d, min %d, max %d, set %d, inverted %d",
                     channels1[i].id, channels1[i].minPos, channels1[i].maxPos, channels1[i].set, channels1[i].inverted);
        }

        pthread_mutex_unlock(&channelsMutex);
    }

    ServoModule::ZeroServos();
}

void ServoModule::QueueCommand(uint8_t *cmd)
{
    ESP_LOGI(TAG, "Queueing servo command => %s", cmd);
    ServoCommand servoCmd = ServoCommand(std::string(reinterpret_cast<char *>(cmd)));

    if (servoCmd.position == 666)
    {
        // this is a test command to set the channel
        // pulse command directly between 0 and 4096
        if (servoCmd.channel < 16)
        {
            pcaBoard0.setPwm(servoCmd.channel, 0, servoCmd.speed);
        }
        else if (servoCmd.channel < 32)
        {
            pcaBoard1.setPwm(servoCmd.channel - 16, 0, servoCmd.speed);
        }
    }
    else if (servoCmd.channel < 16)
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
        int requestPos = cmd->position;

        if (servos[cmd->channel].inverted)
        {
            requestPos = 100 - requestPos;
        }

        servos[cmd->channel].speed = cmd->speed;

        if (requestPos <= 0)
        {
            servos[cmd->channel].requestedPos = servos[cmd->channel].minPos;
            servos[cmd->channel].on = true;
            pthread_mutex_unlock(&channelsMutex);
            return;
        }
        else if (requestPos >= 100)
        {
            servos[cmd->channel].requestedPos = servos[cmd->channel].maxPos;
            servos[cmd->channel].on = true;
            pthread_mutex_unlock(&channelsMutex);
            return;
        }

        int move = servos[cmd->channel].moveFactor * requestPos;
        servos[cmd->channel].requestedPos = servos[cmd->channel].minPos + move;

        if (servos[cmd->channel].requestedPos > servos[cmd->channel].maxPos)
        {
            servos[cmd->channel].requestedPos = servos[cmd->channel].maxPos;
        }

        servos[cmd->channel].on = true;
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
            if (channels0[i].inverted)
            {
                channels0[i].requestedPos = channels0[i].maxPos;
            }
            else
            {
                channels0[i].requestedPos = channels0[i].minPos;
            }
            channels0[i].speed = 5;
            channels0[i].on = true;

            if (channels1[i].inverted)
            {
                channels1[i].requestedPos = channels1[i].maxPos;
            }
            else
            {
                channels1[i].requestedPos = channels1[i].minPos;
            }
            channels1[i].speed = 5;
            channels1[i].on = true;
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
    if (!servos[idx].set)
    {
        if (servos[idx].on)
        {
            board->setPwm(idx, 0, 4096);
            servos[idx].on = false;
            ESP_LOGI(TAG, "Servo is not set. Setting servo channel %d on board %d off", idx, board->getAddress());
        }
        return;
    }

    if (!servos[idx].on)
    {
        return;
    }

    if (servos[idx].currentPos == servos[idx].requestedPos)
    {
        if (servos[idx].on)
        {
            board->setPwm(idx, 0, 4096);
            servos[idx].on = false;
            ESP_LOGI(TAG, "Setting servo channel %d on board %d off", idx, board->getAddress());
        }
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
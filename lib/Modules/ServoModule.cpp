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

#define SERVOMIN 150
#define SERVOMAX 600

static const char *TAG = "ServoModule";

Pca9685 pcaBoard;

static pthread_mutex_t channelsMutex;

const uint16_t pwmTable[256] = {0, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 11, 11, 11, 12, 12, 12, 13, 13, 13, 14, 14, 15, 15, 15, 16, 16, 17, 17, 18, 18, 19, 19, 20, 21, 21, 22, 22, 23, 24, 24, 25, 26, 27, 27, 28, 29, 30, 31, 31, 32, 33, 34, 35, 36, 37, 38, 39, 41, 42, 43, 44, 45, 47, 48, 49, 51, 52, 54, 55, 57, 59, 60, 62, 64, 66, 68, 69, 71, 74, 76, 78, 80, 82, 85, 87, 90, 92, 95, 98, 100, 103, 106, 109, 112, 116, 119, 122, 126, 130, 133, 137, 141, 145, 149, 153, 158, 162, 167, 172, 177, 182, 187, 193, 198, 204, 210, 216, 222, 228, 235, 241, 248, 255, 263, 270, 278, 286, 294, 303, 311, 320, 330, 339, 349, 359, 369, 380, 391, 402, 413, 425, 437, 450, 463, 476, 490, 504, 518, 533, 549, 564, 581, 597, 614, 632, 650, 669, 688, 708, 728, 749, 771, 793, 816, 839, 863, 888, 913, 940, 967, 994, 1023, 1052, 1082, 1114, 1146, 1178, 1212, 1247, 1283, 1320, 1358, 1397, 1437, 1478, 1520, 1564, 1609, 1655, 1703, 1752, 1802, 1854, 1907, 1962, 2018, 2076, 2135, 2197, 2260, 2325, 2391, 2460, 2531, 2603, 2678, 2755, 2834, 2916, 2999, 3085, 3174, 3265, 3359, 3455, 3555, 3657, 3762, 3870, 3981, 4095};

servo_channel channels[16] = {
 /*   {0, 120, 440, 1, 120, 120, 1, true},
    {1, 0, 4096, 1, 0, 0, 1, false},
    {2, 0, 4096, 1, 0, 0, 1, false},
    {3, 0, 4096, 1, 0, 0, 1, false},
    {4, 0, 4096, 1, 0, 0, 1, false},
    {5, 0, 4096, 1, 0, 0, 1, false},
    {6, 0, 4096, 1, 0, 0, 1, false},
    {7, 0, 4096, 1, 0, 0, 1, false},
    {8, 0, 4096, 1, 0, 0, 1, false},
    {9, 0, 4096, 1, 0, 0, 1, false},
    {10, 0, 4096, 1, 0, 0, 1, false},
    {11, 0, 4096, 1, 0, 0, 1, false},
    {12, 0, 4096, 1, 0, 0, 1, false},
    {13, 0, 4096, 1, 0, 0, 1, false},
    {14, 0, 4096, 1, 0, 0, 1, false},
    {15, 0, 4096, 1, 0, 0, 1, false},
    */
};

ServoModule ServoMod;

ServoModule::ServoModule(/* args */)
{
}

ServoModule::~ServoModule()
{
}

esp_err_t ServoModule::Init(i2c_port_t port, gpio_num_t sda, gpio_num_t scl, uint16_t frequency)
{
    esp_err_t result = ESP_OK;

    if (pthread_mutex_init(&channelsMutex, NULL) != 0)
    {
        ESP_LOGE(TAG, "Failed to initialize the channels mutex");
    }

    ServoModule::LoadServoConfig();
    
    result = pcaBoard.Init(port, sda, scl, frequency);

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

        Storage.loadServoConfig(channels, 16);

        for (size_t i = 0; i < 16; i++)
        {
            int x = (channels[i].maxPos - channels[i].minPos) / 100;
            channels[i].moveFactor = (int)(x + 0.5);
            
            ESP_LOGI(TAG, "Servo Config=> ch: %d, min %d, max %d, set %d", channels[i].id, channels[i].minPos, channels[i].maxPos, channels[i].set);
        }

        pthread_mutex_unlock(&channelsMutex);
    }
}

void ServoModule::QueueCommand(const char *cmd)
{
    ESP_LOGI(TAG, "Queueing servo command => %s", cmd);

    ServoCommand servoCmd = ServoCommand(cmd);

    if (pthread_mutex_lock(&channelsMutex) == 0)
    {
        channels[servoCmd.channel].speed = servoCmd.speed;
        
        if (servoCmd.position <= 0)
        {
            channels[servoCmd.channel].requestedPos = channels[servoCmd.channel].minPos;
            pthread_mutex_unlock(&channelsMutex);
            return;
        }

        int move = channels[servoCmd.channel].moveFactor * servoCmd.position;

        channels[servoCmd.channel].requestedPos = channels[servoCmd.channel].minPos + move;
    
        if (channels[servoCmd.channel].requestedPos > channels[servoCmd.channel].maxPos || servoCmd.position >= 100)
        {
            channels[servoCmd.channel].requestedPos = channels[servoCmd.channel].maxPos;
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
            channels[i].requestedPos = channels[i].currentPos;
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
            channels[i].requestedPos = channels[i].minPos;
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
            // only attempt to move servos that have a min/max
            if (!channels[i].set)
            {
                continue;
            }

            if (channels[i].currentPos == channels[i].requestedPos)
            {
                continue;
            }
            else if (channels[i].currentPos < channels[i].requestedPos)
            {

                channels[i].currentPos += channels[i].speed;

                if (channels[i].currentPos > channels[i].requestedPos)
                {
                    channels[i].currentPos = channels[i].requestedPos;
                }
                pcaBoard.setPwm(i, 0, channels[i].currentPos);
            }
            else
            {

                channels[i].currentPos -= channels[i].speed;

                if (channels[i].currentPos < channels[i].requestedPos)
                {
                    channels[i].currentPos = channels[i].requestedPos;
                }
                pcaBoard.setPwm(i, 0, channels[i].currentPos);
            }
        }
        pthread_mutex_unlock(&channelsMutex);
    }
}
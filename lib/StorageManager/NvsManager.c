#include "AstrOsUtility.h"

#include <stdbool.h>
#include <nvs_flash.h>
#include <string.h>

static const char *TAG = "NvsManager";

bool nvsSaveServiceConfig(svc_config_t config)
{
    if (config.networkSSID != NULL &&
        config.networkPass != NULL &&
        config.networkSSID[0] != '\0' &&
        config.networkPass[0] != '\0')
    {
        nvs_handle_t nvsHandle;
        esp_err_t err;

        err = nvs_open("config", NVS_READWRITE, &nvsHandle);

        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            nvs_close(nvsHandle);
            return false;
        }

        err = nvs_set_str(nvsHandle, "networkSSID", config.networkSSID);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            nvs_close(nvsHandle);
            return false;
        }

        err = nvs_set_str(nvsHandle, "networkPass", config.networkPass);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            nvs_close(nvsHandle);
            return false;
        }

        err = nvs_commit(nvsHandle);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            nvs_close(nvsHandle);
            return false;
        }

        nvs_close(nvsHandle);

        return true;
    }
    else
    {
        return false;
    }
}

bool nvsLoadServiceConfig(svc_config_t *config)
{

    strncpy(config->networkSSID, "Interwebs", sizeof(config->networkSSID) - 1);
    strncpy(config->networkPass, "Rubherducky21!", sizeof(config->networkPass) - 1);

    return true;

    esp_err_t err;
    nvs_handle_t nvsHandle;
    size_t defaultSize = 0;
    err = nvs_open("config", NVS_READWRITE, &nvsHandle);

    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    defaultSize = 33;
    err = nvs_get_str(nvsHandle, "networkSSID", config->networkSSID, &defaultSize);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    defaultSize = 65;
    err = nvs_get_str(nvsHandle, "networkPass", config->networkPass, &defaultSize);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    nvs_close(nvsHandle);

    return true;
}

bool nvsClearServiceConfig()
{

    esp_err_t err;
    nvs_handle_t nvsHandle;
    size_t defaultSize = 0;
    err = nvs_open("config", NVS_READWRITE, &nvsHandle);

    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    err = nvs_erase_key(nvsHandle, "networkSSID");
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    err = nvs_erase_key(nvsHandle, "networkPass");
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    err = nvs_commit(nvsHandle);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }
    nvs_close(nvsHandle);
    return true;
}

bool nvsSaveServoConfig(servo_channel *config, int arraySize)
{
    esp_err_t err;
    nvs_handle_t nvsHandle;
    size_t defaultSize = 0;
    err = nvs_open("config", NVS_READWRITE, &nvsHandle);

    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    char minPosConfig[] = "00-minpos";
    char maxPosConfig[] = "00-maxpos";
    char setConfig[] = "00-set"; 

    for (size_t i = 0; i < arraySize; i++)
    {
        if (i < 10){
            minPosConfig[1] = (i + '0');
            maxPosConfig[1] = (i + '0');
            setConfig[1] = (i + '0');
        }
        else
        {
            minPosConfig[0] = (1 + '0');
            minPosConfig[1] = ((i - 10) + '0');
            maxPosConfig[0] = (1 + '0');
            maxPosConfig[1] = ((i - 10) + '0');
            setConfig[0] = (1 + '0');
            setConfig[1] = ((i - 10) + '0');
        }

        err = nvs_set_u16(nvsHandle, minPosConfig, config[i].minPos);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            nvs_close(nvsHandle);
            return false;
        }

        err = nvs_set_u16(nvsHandle, maxPosConfig, config[i].maxPos);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            nvs_close(nvsHandle);
            return false;
        }

        err = nvs_set_u8(nvsHandle, setConfig, config[i].set);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            nvs_close(nvsHandle);
            return false;
        }
    }

    err = nvs_commit(nvsHandle);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }
    nvs_close(nvsHandle);
    return true;
}

bool nvsLoadServoConfig(servo_channel *config, int arraySize)
{

    esp_err_t err;
    nvs_handle_t nvsHandle;
    size_t defaultSize = 0;
    err = nvs_open("config", NVS_READWRITE, &nvsHandle);

    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    uint16_t min;
    uint16_t max;
    bool set;

    char minPosConfig[] = "00-minpos";
    char maxPosConfig[] = "00-maxpos";
    char setConfig[] = "00-set"; 

    for (size_t i = 0; i < arraySize; i++)
    {
        if (i < 10){
            minPosConfig[1] = (i + '0');
            maxPosConfig[1] = (i + '0');
            setConfig[1] = (i + '0');
        }
        else
        {
            minPosConfig[0] = (1 + '0');
            minPosConfig[1] = ((i - 10) + '0');
            maxPosConfig[0] = (1 + '0');
            maxPosConfig[1] = ((i - 10) + '0');
            setConfig[0] = (1 + '0');
            setConfig[1] = ((i - 10) + '0');
        }

        err = nvs_get_u16(nvsHandle, minPosConfig, &min);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            min = 0;
        }
        err = nvs_get_u16(nvsHandle, maxPosConfig, &max);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            max = 4096;
        }
        err = nvs_get_u8(nvsHandle, setConfig, &set);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            set = false;
        }

        servo_channel channel;
        
        channel.id = i;
        channel.minPos = min;
        channel.maxPos = max;
        channel.set = set;
        channel.currentPos = min;
        channel.requestedPos = min;
        channel.moveFactor = 1;
        channel.speed = 1;

        config[i] = channel;
    }

    nvs_close(nvsHandle);
    return true;
}

#include "AstrOsUtility.h"

#include <stdbool.h>
#include <nvs_flash.h>
#include <string.h>
#include <math.h>

static const char *TAG = "NvsManager";

bool nvsSaveServiceConfig(svc_config_t config)
{
    if (config.networkSSID[0] != '\0' &&
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

// sets the configuration fingerprint to ensure config is up to date
bool nvsSetControllerFingerprint(const char *fingerprint)
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

    err = nvs_set_str(nvsHandle, "fingerprint", fingerprint);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    nvs_close(nvsHandle);
    return true;
}

bool nvsGetControllerFingerprint(char *fingerprint)
{
    esp_err_t err;
    nvs_handle_t nvsHandle;
    size_t defaultSize = 37;
    err = nvs_open("config", NVS_READWRITE, &nvsHandle);

    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    err = nvs_get_str(nvsHandle, "fingerprint", fingerprint, &defaultSize);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    nvs_close(nvsHandle);
    return true;
}

bool nvsSaveServoConfig(int boardId, servo_channel *config, int arraySize)
{
    esp_err_t err;
    nvs_handle_t nvsHandle;

    err = nvs_open("config", NVS_READWRITE, &nvsHandle);

    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    char minPosConfig[] = "x-00-minpos";
    char maxPosConfig[] = "x-00-maxpos";
    char setConfig[] = "x-00-set";
    char invertedConfig[] = "x-00-inv";

    minPosConfig[0] = (boardId + '0');
    maxPosConfig[0] = (boardId + '0');
    setConfig[0] = (boardId + '0');
    invertedConfig[0] = (boardId + '0');

    for (size_t i = 0; i < arraySize; i++)
    {
        if (i < 10)
        {
            minPosConfig[3] = (i + '0');
            maxPosConfig[3] = (i + '0');
            setConfig[3] = (i + '0');
            invertedConfig[3] = (i + '0');
        }
        else
        {
            minPosConfig[2] = (1 + '0');
            minPosConfig[3] = ((i - 10) + '0');
            maxPosConfig[2] = (1 + '0');
            maxPosConfig[3] = ((i - 10) + '0');
            setConfig[2] = (1 + '0');
            setConfig[3] = ((i - 10) + '0');
            invertedConfig[2] = (1 + '0');
            invertedConfig[3] = ((i - 10) + '0');
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

        err = nvs_set_u8(nvsHandle, invertedConfig, config[i].inverted);
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

bool nvsLoadServoConfig(int boardId, servo_channel *config, int arraySize)
{

    esp_err_t err;
    nvs_handle_t nvsHandle;

    err = nvs_open("config", NVS_READWRITE, &nvsHandle);

    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    uint16_t min;
    uint16_t max;
    bool set;
    bool inverted;

    char minPosConfig[] = "x-00-minpos";
    char maxPosConfig[] = "x-00-maxpos";
    char setConfig[] = "x-00-set";
    char invertedConfig[] = "x-00-inv";

    minPosConfig[0] = (boardId + '0');
    maxPosConfig[0] = (boardId + '0');
    setConfig[0] = (boardId + '0');
    invertedConfig[0] = (boardId + '0');

    for (size_t i = 0; i < arraySize; i++)
    {
        if (i < 10)
        {
            minPosConfig[3] = (i + '0');
            maxPosConfig[3] = (i + '0');
            setConfig[3] = (i + '0');
            invertedConfig[3] = (i + '0');
        }
        else
        {
            minPosConfig[2] = (1 + '0');
            minPosConfig[3] = ((i - 10) + '0');
            maxPosConfig[2] = (1 + '0');
            maxPosConfig[3] = ((i - 10) + '0');
            setConfig[2] = (1 + '0');
            setConfig[3] = ((i - 10) + '0');
            invertedConfig[2] = (1 + '0');
            invertedConfig[3] = ((i - 10) + '0');
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
        err = nvs_get_u8(nvsHandle, invertedConfig, &inverted);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            inverted = false;
        }

        servo_channel channel;

        channel.id = i;
        channel.minPos = min;
        channel.maxPos = max;
        channel.set = set;
        channel.inverted = inverted;
        channel.currentPos = channel.minPos;
        channel.requestedPos = channel.minPos;
        channel.moveFactor = 1;
        channel.speed = 1;

        config[i] = channel;
    }

    nvs_close(nvsHandle);
    return true;
}

#include "AstrOsUtility.h"

#include <stdbool.h>
#include <nvs_flash.h>
#include <string.h>

static const char *TAG = "NvsManager";

bool nvsSaveServiceConfig(svc_config_t config){
    if (config.networkSSID != NULL &&
        config.networkPass != NULL &&
        config.networkSSID[0] != '\0' &&
        config.networkPass[0] != '\0')
    {
        nvs_handle_t nvsHandle;
        esp_err_t err;

        err = nvs_open("config", NVS_READWRITE, &nvsHandle);
        if (logError(TAG, __FUNCTION__, __LINE__, err)){
            return false;
        }

        err = nvs_set_str(nvsHandle, "networkSSID", config.networkSSID);
        if (logError(TAG, __FUNCTION__, __LINE__, err)){
            return false;
        }

        err = nvs_set_str(nvsHandle, "networkPass", config.networkPass);
        if (logError(TAG, __FUNCTION__, __LINE__, err)){
            return false;
        }

        err = nvs_commit(nvsHandle);
        if (logError(TAG, __FUNCTION__, __LINE__, err)){
            return false;
        }
        
        nvs_close(nvsHandle);
        
        return true;
    } else {
        return false;
    }
}

bool nvsLoadServiceConfig(svc_config_t* config){

    strncpy(config->networkSSID, "Interwebs", sizeof(config->networkSSID) - 1);
    strncpy(config->networkPass, "Rubherducky21!", sizeof(config->networkPass) - 1);

    return true;

    esp_err_t err;
    nvs_handle_t nvsHandle;
    size_t defaultSize = 0;
    err = nvs_open("config", NVS_READWRITE, &nvsHandle);

    if (logError(TAG, __FUNCTION__, __LINE__, err)){
        return false;
    }

    defaultSize = 33;
    err = nvs_get_str(nvsHandle, "networkSSID", config->networkSSID, &defaultSize);
    if (logError(TAG, __FUNCTION__, __LINE__, err)){
        return false;
    }
 
    defaultSize = 65;
    err = nvs_get_str(nvsHandle, "networkPass", config->networkPass, &defaultSize);
    if (logError(TAG, __FUNCTION__, __LINE__, err)){
        return false;
    }

    return true;
}

bool nvsClearServiceConfig(){

    esp_err_t err;
    nvs_handle_t nvsHandle;
    size_t defaultSize = 0;
    err = nvs_open("config", NVS_READWRITE, &nvsHandle);

    if (logError(TAG, __FUNCTION__, __LINE__, err)){
        return false;
    }

    err = nvs_erase_key(nvsHandle, "networkSSID");
    if (logError(TAG, __FUNCTION__, __LINE__, err)){
        return false;
    }
 
    err = nvs_erase_key(nvsHandle, "networkPass");
    if (logError(TAG, __FUNCTION__, __LINE__, err)){
        return false;
    }

    nvs_commit(nvsHandle);
    return true;
}

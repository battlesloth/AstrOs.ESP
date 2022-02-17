#include "AstrOsUtility.h"

#include <stdio.h>
#include <stdbool.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <string.h>


static const char *TAG = "AstrOsUtility";

bool logError(const char* tag, const char* function, int line, esp_err_t err){
    if (err != ESP_OK){
        ESP_LOGE(tag, "Error in %s:%d => %s", function, line, esp_err_to_name(err));
        return true;
    }
    return false;
}


bool saveServiceConfig(svc_config_t config){
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

bool loadServiceConfig(svc_config_t* config){

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

bool clearServiceConfig(){

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



bool formatSd(){
    return true;
}

bool readSd(){
    return true;
}

int percentDecode(char* out, const char* in)
{
    static const signed char tbl[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
         0, 1, 2, 3, 4, 5, 6, 7,  8, 9,-1,-1,-1,-1,-1,-1,
        -1,10,11,12,13,14,15,-1, -1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
        -1,10,11,12,13,14,15,-1, -1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1
    };
    char c, v1, v2, *beg=out;
    if(in != NULL) {
        while((c=*in++) != '\0') {
            if(c == '%') {
                if((v1=tbl[(unsigned char)*in++])<0 || 
                   (v2=tbl[(unsigned char)*in++])<0) {
                    *beg = '\0';
                    return -1;
                }
                c = (v1<<4)|v2;
            }
            *out++ = c;
        }
    }
    *out = '\0';
    return 0;
}
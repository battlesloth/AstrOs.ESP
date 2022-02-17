#ifndef STORAGEMANAGER_H
#define STORAGEMANAGER_H

#include <esp_log.h>

#include <AstrOsUtility.h>

class StorageManager{
    private:
        esp_err_t mountSdCard();
    public:
    StorageManager();
    ~StorageManager();
    esp_err_t Init();
    bool loadServiceConfig(svc_config_t* config);
    bool saveServiceConfig(svc_config_t config);
    bool clearServiceConfig();
    
    bool formatSdCard();
    bool readSdCard();
};

extern StorageManager Storage;

#endif
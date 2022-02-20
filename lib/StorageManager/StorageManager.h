#ifndef STORAGEMANAGER_H
#define STORAGEMANAGER_H

#include <esp_log.h>

#include <AstrOsUtility.h>

class StorageManager{
    private:
        esp_err_t mountSdCard();
        char* setFilePath(char* filename);
    public:
    StorageManager();
    ~StorageManager();
    esp_err_t Init();
    bool loadServiceConfig(svc_config_t* config);
    bool saveServiceConfig(svc_config_t config);
    bool clearServiceConfig();
    
    
    bool saveFile(char* filename, char* data);
    bool deleteFile(char* filename);
    bool fileExists(char* filename);

    char* readFile(char* filename);
    bool listFiles(char* folder);

    bool formatSdCard();
    
};

extern StorageManager Storage;

#endif
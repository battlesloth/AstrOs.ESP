#ifndef STORAGEMANAGER_H
#define STORAGEMANAGER_H

#include <esp_log.h>
#include <string>

#include <AstrOsUtility.h>

class StorageManager{
    private:
        esp_err_t mountSdCard();
        std::string setFilePath(std::string filename);
    public:
    StorageManager();
    ~StorageManager();
    esp_err_t Init();
    
    bool loadServiceConfig(svc_config_t* config);
    bool saveServiceConfig(svc_config_t config);
    bool clearServiceConfig();

    bool setControllerFingerprint(const char* fingerprint);
    bool getControllerFingerprint(char* fingerprint);

    bool saveServoConfig(int boardId, servo_channel *servos, int arraySize);
    bool loadServoConfig(int boardId, servo_channel *servos, int arraySize);
    
    bool saveFile(std::string filename, std::string data);
    bool deleteFile(std::string filename);
    bool fileExists(std::string filename);

    std::string readFile(std::string filename);
    bool listFiles(std::string folder);

    bool formatSdCard();
    
};

extern StorageManager Storage;

#endif
#ifndef ASTROSSTORAGEMANAGER_HPP
#define ASTROSSTORAGEMANAGER_HPP

#include <esp_log.h>
#include <string>
#include <vector>

#include <AstrOsUtility.h>
#include <AstrOsEspNowUtility.h>

class AstrOsStorageManager
{
private:
    bool saveMaestroServos(std::vector<std::string> config);
    bool saveMaestroModules(std::vector<std::string> config);
    bool saveGpioConfig(std::string config);
    esp_err_t mountSdCard();
    std::string setFilePath(std::string filename);
    bool saveFileSd(std::string filename, std::string data);
    bool deleteFileSd(std::string filename);
    bool fileExistsSd(std::string filename);
    std::string readFileSd(std::string filename);
    std::vector<std::string> listFilesSd(std::string folder);
    bool saveFileSpiffs(std::string filename, std::string data);
    bool deleteFileSpiffs(std::string filename);
    bool fileExistsSpiffs(std::string filename);
    std::string readFileSpiffs(std::string filename);
    std::vector<std::string> listFilesSpiffs(std::string folder);

public:
    AstrOsStorageManager();
    ~AstrOsStorageManager();
    esp_err_t Init();

    bool loadServiceConfig(svc_config_t *config);
    bool saveServiceConfig(svc_config_t config);
    bool clearServiceConfig();

    bool setControllerFingerprint(const char *fingerprint);
    bool getControllerFingerprint(char *fingerprint);

    bool saveModuleConfigs(std::string msg);

    std::vector<maestro_config> loadMaestroConfigs();
    bool loadMaestroServos(int idx, servo_channel *servos, int arraySize);

    std::vector<bool> loadGpioConfigs();

    bool saveEspNowPeer(espnow_peer_t config);
    bool saveEspNowPeerConfigs(espnow_peer_t *config, int arraySize);
    int loadEspNowPeerConfigs(espnow_peer_t *config);

    bool saveFile(std::string filename, std::string data);
    bool deleteFile(std::string filename);
    bool fileExists(std::string filename);

    std::string readFile(std::string filename);

    std::vector<std::string> listFiles(std::string folder);

    bool formatSdCard();
};

extern AstrOsStorageManager AstrOs_Storage;

#endif
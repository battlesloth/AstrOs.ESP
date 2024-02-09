#include <AstrOsUtility_c.h>
#include <AstrOsEspNow_c.h>
#include <AstrOsUtility_Esp.h>

#include <stdbool.h>
#include <nvs_flash.h>
#include <string.h>
#include <math.h>

static const char *TAG = "NvsManager";

static void setKeyId(char *key, uint8_t id, uint8_t startPos)
{
    if (id < 10)
    {
        key[startPos + 1] = (id + '0');
    }
    else
    {
        key[2] = (1 + '0');
        key[3] = ((id - 10) + '0');
    }
}

bool nvsSaveServiceConfig(svc_config_t config)
{

    nvs_handle_t nvsHandle;
    esp_err_t err;

    err = nvs_open("config", NVS_READWRITE, &nvsHandle);

    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    err = nvs_set_blob(nvsHandle, "masterMac", config.masterMacAddress, 6);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    err = nvs_set_str(nvsHandle, "name", config.name);
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

bool nvsLoadServiceConfig(svc_config_t *config)
{
    esp_err_t err;
    nvs_handle_t nvsHandle;
    size_t defaultSize = 0;
    bool result = true;

    err = nvs_open("config", NVS_READWRITE, &nvsHandle);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    defaultSize = 6;
    err = nvs_get_blob(nvsHandle, "masterMac", config->masterMacAddress, &defaultSize);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        memset(config->masterMacAddress, 255, 6);
        result = false;
    }

    defaultSize = 16;
    err = nvs_get_str(nvsHandle, "name", config->name, &defaultSize);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        memcpy(config->name, "AstrOs\0", 7);
        result = false;
    }

    nvs_close(nvsHandle);

    return result;
}

bool nvsClearServiceConfig()
{
    esp_err_t err;
    nvs_handle_t nvsHandle;

    err = nvs_open("config", NVS_READWRITE, &nvsHandle);

    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    err = nvs_erase_key(nvsHandle, "masterMac");
    logError(TAG, __FUNCTION__, __LINE__, err);

    err = nvs_erase_key(nvsHandle, "name");
    logError(TAG, __FUNCTION__, __LINE__, err);

    err = nvs_commit(nvsHandle);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    int8_t peers = 0;
    char peerCountConfig[] = "peer-count";

    err = nvs_get_i8(nvsHandle, peerCountConfig, &peers);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    char nameConfig[] = "p-00-name";
    char macConfig[] = "p-00-mac";
    char cryptoKeyConfig[] = "p-00-crypt";
    char isPairedConfig[] = "p-00-paired";

    for (size_t i = 0; i < peers; i++)
    {
        setKeyId(nameConfig, i, 2);
        setKeyId(macConfig, i, 2);
        setKeyId(cryptoKeyConfig, i, 2);
        setKeyId(isPairedConfig, i, 2);

        err = nvs_erase_key(nvsHandle, nameConfig);
        logError(TAG, __FUNCTION__, __LINE__, err);

        err = nvs_erase_key(nvsHandle, macConfig);
        logError(TAG, __FUNCTION__, __LINE__, err);

        err = nvs_erase_key(nvsHandle, cryptoKeyConfig);
        logError(TAG, __FUNCTION__, __LINE__, err);

        err = nvs_erase_key(nvsHandle, isPairedConfig);
        logError(TAG, __FUNCTION__, __LINE__, err);
    }

    err = nvs_erase_key(nvsHandle, peerCountConfig);
    logError(TAG, __FUNCTION__, __LINE__, err);

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
        setKeyId(minPosConfig, i, 2);
        setKeyId(maxPosConfig, i, 2);
        setKeyId(setConfig, i, 2);
        setKeyId(invertedConfig, i, 2);

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
    uint8_t set;
    uint8_t inverted;

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
        setKeyId(minPosConfig, i, 2);
        setKeyId(maxPosConfig, i, 2);
        setKeyId(setConfig, i, 2);
        setKeyId(invertedConfig, i, 2);

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
            set = 0;
        }
        err = nvs_get_u8(nvsHandle, invertedConfig, &inverted);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            inverted = 0;
        }

        servo_channel channel;

        channel.id = i;
        channel.minPos = min;
        channel.maxPos = max;
        channel.set = set > 0;
        channel.inverted = inverted > 0;
        channel.currentPos = channel.minPos;
        channel.requestedPos = channel.minPos;
        channel.moveFactor = 1;
        channel.speed = 1;

        config[i] = channel;
    }

    nvs_close(nvsHandle);
    return true;
}

bool nvsSaveEspNowPeer(espnow_peer_t config)
{
    esp_err_t err;
    nvs_handle_t nvsHandle;

    err = nvs_open("config", NVS_READWRITE, &nvsHandle);

    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    int8_t peers = 0;

    char peerCountConfig[] = "peer-count";

    err = nvs_get_i8(nvsHandle, peerCountConfig, &peers);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        if (err == ESP_ERR_NVS_NOT_FOUND)
        {
            peers = 0;
        }
        else
        {
            nvs_close(nvsHandle);
            return false;
        }
    }

    peers++;

    err = nvs_set_i8(nvsHandle, peerCountConfig, peers);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    char nameConfig[] = "p-00-name";
    char macConfig[] = "p-00-mac";
    char cryptoKeyConfig[] = "p-00-crypt";
    char isPairedConfig[] = "p-00-paired";

    int i = config.id;

    setKeyId(nameConfig, i, 2);
    setKeyId(macConfig, i, 2);
    setKeyId(cryptoKeyConfig, i, 2);
    setKeyId(isPairedConfig, i, 2);

    err = nvs_set_str(nvsHandle, nameConfig, config.name);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    err = nvs_set_blob(nvsHandle, macConfig, config.mac_addr, 6);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    err = nvs_set_str(nvsHandle, cryptoKeyConfig, config.crypto_key);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    err = nvs_set_u8(nvsHandle, isPairedConfig, config.is_paired);
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

bool nvsSaveEspNowPeerConfigs(espnow_peer_t *config, int arraySize)
{
    esp_err_t err;
    nvs_handle_t nvsHandle;

    err = nvs_open("config", NVS_READWRITE, &nvsHandle);

    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    int peers = arraySize;

    char peerCountConfig[] = "peer-count";

    err = nvs_set_i8(nvsHandle, peerCountConfig, peers);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }
    char nameConfig[] = "p-00-name";
    char macConfig[] = "p-00-mac";
    char cryptoKeyConfig[] = "p-00-crypt";
    char isPairedConfig[] = "p-00-paired";

    for (size_t i = 0; i < peers; i++)
    {
        setKeyId(nameConfig, i, 2);
        setKeyId(macConfig, i, 2);
        setKeyId(cryptoKeyConfig, i, 2);
        setKeyId(isPairedConfig, i, 2);

        err = nvs_set_str(nvsHandle, nameConfig, config[i].name);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            nvs_close(nvsHandle);
            return false;
        }

        err = nvs_set_blob(nvsHandle, macConfig, config[i].mac_addr, 6);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            nvs_close(nvsHandle);
            return false;
        }

        err = nvs_set_str(nvsHandle, cryptoKeyConfig, config[i].crypto_key);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            nvs_close(nvsHandle);
            return false;
        }

        err = nvs_set_u8(nvsHandle, isPairedConfig, config[i].is_paired);
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

int nvsLoadEspNowPeerConfigs(espnow_peer_t *config)
{

    esp_err_t err;
    nvs_handle_t nvsHandle;

    err = nvs_open("config", NVS_READWRITE, &nvsHandle);

    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return 0;
    }

    int8_t peers = 0;
    char peerCountConfig[] = "peer-count";

    err = nvs_get_i8(nvsHandle, peerCountConfig, &peers);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return 0;
    }

    size_t size = 0;
    char name[16];
    uint8_t mac[6];
    char cryptoKey[16];
    uint8_t isPaired;

    char nameConfig[] = "p-00-name";
    char macConfig[] = "p-00-mac";
    char cryptoKeyConfig[] = "p-00-crypt";
    char isPairedConfig[] = "p-00-paired";

    for (size_t i = 0; i < peers; i++)
    {
        setKeyId(nameConfig, i, 2);
        setKeyId(macConfig, i, 2);
        setKeyId(cryptoKeyConfig, i, 2);
        setKeyId(isPairedConfig, i, 2);

        size = 16;
        err = nvs_get_str(nvsHandle, nameConfig, name, &size);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            memset(name, 0, 16);
        }

        size = 6;
        err = nvs_get_blob(nvsHandle, macConfig, mac, &size);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            memset(mac, 0, 6);
        }

        size = 16;
        err = nvs_get_str(nvsHandle, cryptoKeyConfig, cryptoKey, &size);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            memset(cryptoKey, 0, 16);
        }

        err = nvs_get_u8(nvsHandle, isPairedConfig, &isPaired);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            isPaired = 0;
        }

        espnow_peer_t peer;

        peer.id = 1;
        memcpy(peer.name, name, 16);
        memcpy(peer.mac_addr, mac, 6);
        memcpy(peer.crypto_key, cryptoKey, 16);
        peer.is_paired = isPaired > 0;

        config[i] = peer;
    }

    nvs_close(nvsHandle);
    return peers;
}

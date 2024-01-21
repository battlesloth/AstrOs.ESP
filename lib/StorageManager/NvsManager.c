#include "AstrOsUtility.h"
#include "AstrOsEspNowHelpers.h"

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

bool nvsSaveMasterMacAddress(uint8_t *mac)
{
    esp_err_t err;
    nvs_handle_t nvsHandle;

    err = nvs_open("config", NVS_READWRITE, &nvsHandle);

    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    err = nvs_set_blob(nvsHandle, "masterMac", mac, 6);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }
    nvs_close(nvsHandle);
    return true;
}

bool nvsLoadMasterMacAddress(uint8_t *mac)
{
    esp_err_t err;
    nvs_handle_t nvsHandle;
    size_t defaultSize = 6;

    err = nvs_open("config", NVS_READWRITE, &nvsHandle);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        memset(mac, 255, 6);
        nvs_close(nvsHandle);
        return false;
    }

    err = nvs_get_blob(nvsHandle, "masterMac", mac, &defaultSize);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        memset(mac, 255, 6);
        nvs_close(nvsHandle);
        return false;
    }

    nvs_close(nvsHandle);
    return true;
}

bool nvsClearEspNowPeerConfig()
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

    int peers = 0;
    char peerCountConfig[] = "peer-count";

    err = nvs_get_u8(nvsHandle, peerCountConfig, &peers);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    char peerNameConfig[] = "peer-00-name";
    char peerMacConfig[] = "peer-00-mac";
    char peerChannelConfig[] = "peer-00-channel";
    char peerCryptoKeyConfig[] = "peer-00-cryptokey";
    char peerIsPairedConfig[] = "peer-00-ispaired";

    for (size_t i = 0; i < peers; i++)
    {
        if (i < 10)
        {
            peerNameConfig[6] = (i + '0');
            peerMacConfig[6] = (i + '0');
            peerChannelConfig[6] = (i + '0');
            peerCryptoKeyConfig[6] = (i + '0');
            peerIsPairedConfig[6] = (i + '0');
        }
        else
        {
            peerNameConfig[5] = (1 + '0');
            peerNameConfig[6] = ((i - 10) + '0');
            peerMacConfig[5] = (1 + '0');
            peerMacConfig[6] = ((i - 10) + '0');
            peerChannelConfig[5] = (1 + '0');
            peerChannelConfig[6] = ((i - 10) + '0');
            peerCryptoKeyConfig[5] = (1 + '0');
            peerCryptoKeyConfig[6] = ((i - 10) + '0');
            peerIsPairedConfig[5] = (1 + '0');
            peerIsPairedConfig[6] = ((i - 10) + '0');
        }

        err = nvs_erase_key(nvsHandle, peerNameConfig);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            nvs_close(nvsHandle);
            return false;
        }

        err = nvs_erase_key(nvsHandle, peerMacConfig);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            nvs_close(nvsHandle);
            return false;
        }

        err = nvs_erase_key(nvsHandle, peerChannelConfig);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            nvs_close(nvsHandle);
            return false;
        }

        err = nvs_erase_key(nvsHandle, peerChannelConfig);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            nvs_close(nvsHandle);
            return false;
        }

        err = nvs_erase_key(nvsHandle, peerCryptoKeyConfig);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            nvs_close(nvsHandle);
            return false;
        }

        err = nvs_erase_key(nvsHandle, peerIsPairedConfig);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            nvs_close(nvsHandle);
            return false;
        }
    }

    err = nvs_erase_key(nvsHandle, peerCountConfig);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
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

    int peers = 0;

    char peerCountConfig[] = "peer-count";

    err = nvs_get_u8(nvsHandle, peerCountConfig, &peers);
    logError(TAG, __FUNCTION__, __LINE__, err);

    peers++;

    err = nvs_set_u8(nvsHandle, peerCountConfig, peers);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    char peerNameConfig[] = "peer-00-name";
    char peerMacConfig[] = "peer-00-mac";
    char peerCryptoKeyConfig[] = "peer-00-cryptokey";
    char peerIsPairedConfig[] = "peer-00-ispaired";

    int i = config.id;

    if (i < 10)
    {
        peerNameConfig[6] = (i + '0');
        peerMacConfig[6] = (i + '0');
        peerCryptoKeyConfig[6] = (i + '0');
        peerIsPairedConfig[6] = (i + '0');
    }
    else
    {
        peerNameConfig[5] = (1 + '0');
        peerNameConfig[6] = ((i - 10) + '0');
        peerMacConfig[5] = (1 + '0');
        peerMacConfig[6] = ((i - 10) + '0');
        peerCryptoKeyConfig[5] = (1 + '0');
        peerCryptoKeyConfig[6] = ((i - 10) + '0');
        peerIsPairedConfig[5] = (1 + '0');
        peerIsPairedConfig[6] = ((i - 10) + '0');
    }

    err = nvs_set_blob(nvsHandle, peerNameConfig, config.name, 16);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    err = nvs_set_blob(nvsHandle, peerMacConfig, config.mac_addr, 6);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    err = nvs_set_blob(nvsHandle, peerCryptoKeyConfig, config.crypto_key, 16);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    err = nvs_set_u8(nvsHandle, peerIsPairedConfig, config.is_paired);
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

    err = nvs_set_u8(nvsHandle, peerCountConfig, peers);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return false;
    }

    char peerNameConfig[] = "peer-00-name";
    char peerMacConfig[] = "peer-00-mac";
    char peerCryptoKeyConfig[] = "peer-00-cryptokey";
    char peerIsPairedConfig[] = "peer-00-ispaired";

    for (size_t i = 0; i < peers; i++)
    {
        if (i < 10)
        {
            peerNameConfig[6] = (i + '0');
            peerMacConfig[6] = (i + '0');
            peerCryptoKeyConfig[6] = (i + '0');
            peerIsPairedConfig[6] = (i + '0');
        }
        else
        {
            peerNameConfig[5] = (1 + '0');
            peerNameConfig[6] = ((i - 10) + '0');
            peerMacConfig[5] = (1 + '0');
            peerMacConfig[6] = ((i - 10) + '0');
            peerCryptoKeyConfig[5] = (1 + '0');
            peerCryptoKeyConfig[6] = ((i - 10) + '0');
            peerIsPairedConfig[5] = (1 + '0');
            peerIsPairedConfig[6] = ((i - 10) + '0');
        }

        err = nvs_set_blob(nvsHandle, peerNameConfig, config[i].name, 16);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            nvs_close(nvsHandle);
            return false;
        }

        err = nvs_set_blob(nvsHandle, peerMacConfig, config[i].mac_addr, 6);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            nvs_close(nvsHandle);
            return false;
        }

        err = nvs_set_blob(nvsHandle, peerCryptoKeyConfig, config[i].crypto_key, 16);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            nvs_close(nvsHandle);
            return false;
        }

        err = nvs_set_u8(nvsHandle, peerIsPairedConfig, config[i].is_paired);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            nvs_close(nvsHandle);
            return false;
        }
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

    int peers = 0;
    char peerCountConfig[] = "peer-count";

    err = nvs_get_u8(nvsHandle, peerCountConfig, &peers);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        nvs_close(nvsHandle);
        return 0;
    }

    uint8_t name[16];
    uint8_t mac[6];
    uint8_t channel;
    uint8_t cryptoKey[16];
    bool isPaired;

    char peerNameConfig[] = "peer-00-name";
    char peerMacConfig[] = "peer-00-mac";
    char peerCryptoKeyConfig[] = "peer-00-cryptokey";
    char peerIsPairedConfig[] = "peer-00-ispaired";

    for (size_t i = 0; i < peers; i++)
    {
        if (i < 10)
        {
            peerNameConfig[6] = (i + '0');
            peerMacConfig[6] = (i + '0');
            peerCryptoKeyConfig[6] = (i + '0');
            peerIsPairedConfig[6] = (i + '0');
        }
        else
        {
            peerNameConfig[5] = (1 + '0');
            peerNameConfig[6] = ((i - 10) + '0');
            peerMacConfig[5] = (1 + '0');
            peerMacConfig[6] = ((i - 10) + '0');
            peerCryptoKeyConfig[5] = (1 + '0');
            peerCryptoKeyConfig[6] = ((i - 10) + '0');
            peerIsPairedConfig[5] = (1 + '0');
            peerIsPairedConfig[6] = ((i - 10) + '0');
        }

        err = nvs_get_blob(nvsHandle, peerNameConfig, name, 16);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            memset(name, 0, 16);
        }

        err = nvs_get_blob(nvsHandle, peerMacConfig, mac, 6);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            memset(mac, 0, 6);
        }

        err = nvs_get_blob(nvsHandle, peerCryptoKeyConfig, cryptoKey, 16);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            memset(cryptoKey, 0, 16);
        }

        err = nvs_get_u8(nvsHandle, peerIsPairedConfig, &isPaired);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            isPaired = false;
        }

        espnow_peer_t peer;

        peer.id = 1;
        memcpy(peer.name, name, 16);
        memcpy(peer.mac_addr, mac, 6);
        memcpy(peer.crypto_key, cryptoKey, 16);
        peer.is_paired = isPaired;

        config[i] = peer;
    }

    nvs_close(nvsHandle);
    return peers;
}
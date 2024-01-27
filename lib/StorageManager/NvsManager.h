#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#include <AstrOsUtility.h>
#include <AstrOsEspNow_c.h>

    // save the service configuration to NVS
    bool nvsSaveServiceConfig(svc_config_t config);

    // load the service configuration from NVS
    bool nvsLoadServiceConfig(svc_config_t *config);

    // clear the service configuration from NVS
    bool nvsClearServiceConfig();

    // sets the configuration fingerprint to ensure config is up to date
    bool nvsSetControllerFingerprint(const char *fingerprint);

    // gets the configuration fingerprint to ensure config is up to date
    bool nvsGetControllerFingerprint(char *fingerprint);

    // saves the servo config
    bool nvsSaveServoConfig(int boardId, servo_channel *config, int arraySize);

    // gets the servo config
    bool nvsLoadServoConfig(int boardId, servo_channel *config, int arraySize);

    // saves the master mac address
    bool nvsSaveMasterMacAddress(uint8_t *mac);

    // gets the master mac address
    bool nvsLoadMasterMacAddress(uint8_t *mac);

    // clear all configured peers
    bool nvsClearEspNowPeerConfig();

    // saves single espnow peer config
    bool nvsSaveEspNowPeer(espnow_peer_t config);

    // saves the espnow peer configs
    bool nvsSaveEspNowPeerConfigs(espnow_peer_t *config, int arraySize);

    // gets the espnow peer configs
    int nvsLoadEspNowPeerConfigs(espnow_peer_t *config);

#ifdef __cplusplus
}
#endif
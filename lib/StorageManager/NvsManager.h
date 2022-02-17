#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <AstrOsUtility.h>


// save the service configuration to NVS
bool nvsSaveServiceConfig(svc_config_t config);

// load the service configuration from NVS
bool nvsLoadServiceConfig(svc_config_t* config);

// clear the service configuration from NVS
bool nvsClearServiceConfig();


#ifdef __cplusplus
}
#endif
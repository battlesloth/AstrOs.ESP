#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdbool.h>
#include <esp_err.h>

typedef enum {
    START_WIFI_AP,
    STOP_WIFI_AP,
    CONNECT_TO_NETWORK,
    DISCONNECT_FROM_NETWORK,
    SWITCH_TO_NETWORK,
    SWITCH_TO_WIFI_AP
} SERVICE_COMMAND;


typedef enum {
    CONFIG,
    FUNCTION,
    SCRIPT,
    I2C,
    SERVO,
} ASTROS_COMMAND;


typedef struct {
    char networkSSID[33];
    char networkPass[65];
} svc_config_t;

typedef struct {
    ASTROS_COMMAND command;
    uint8_t *body;
} serial_cmd_t;


typedef struct {
    SERVICE_COMMAND cmd;
    char *data;
} queue_cmd_t;


typedef struct {
    int message_id;
    char *data;
} queue_msg_t;


// if the esp_err_t != ESP_OK, log the error with the function and line number
bool logError(const char* tag, const char* function, int line, esp_err_t err);

// save the service configuration to NVS
bool saveServiceConfig(svc_config_t config);

// load the service configuration from NVS
bool loadServiceConfig(svc_config_t* config);

// clear the service configuration from NVS
bool clearServiceConfig();

// mount SD Card
bool mountSd();

// format the SD Card
bool formatSd();

// read the SD Card
bool readSd();

// decode url encoded strings
int percentDecode(char* out, const char* in);

#ifdef __cplusplus
}
#endif
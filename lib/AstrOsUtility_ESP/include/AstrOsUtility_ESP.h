
#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#include <esp_log.h>
#include <esp_err.h>
#include <stdbool.h>

    // if the esp_err_t != ESP_OK, log the error with the function and line number
    bool logError(const char *tag, const char *function, int line, esp_err_t err);

#ifdef __cplusplus
}
#endif
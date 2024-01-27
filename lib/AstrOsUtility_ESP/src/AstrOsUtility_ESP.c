#include "AstrOsUtility_ESP.h"

#include <esp_log.h>
#include <esp_err.h>

// if the esp_err_t != ESP_OK, log the error with the function and line number
bool logError(const char *tag, const char *function, int line, esp_err_t err)
{
    if (err != ESP_OK)
    {
        ESP_LOGE(tag, "Error in %s:%d => %s", function, line, esp_err_to_name(err));
        return true;
    }
    return false;
}
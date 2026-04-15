#include "AstrOsLoggerEsp.h"

#include <esp_log.h>

namespace
{
    void espLogVerbose(const char *tag, const char *fmt, va_list args)
    {
        esp_log_writev(ESP_LOG_VERBOSE, tag, fmt, args);
    }

    void espLogDebug(const char *tag, const char *fmt, va_list args)
    {
        esp_log_writev(ESP_LOG_DEBUG, tag, fmt, args);
    }

    void espLogInfo(const char *tag, const char *fmt, va_list args)
    {
        esp_log_writev(ESP_LOG_INFO, tag, fmt, args);
    }

    void espLogWarn(const char *tag, const char *fmt, va_list args)
    {
        esp_log_writev(ESP_LOG_WARN, tag, fmt, args);
    }

    void espLogError(const char *tag, const char *fmt, va_list args)
    {
        esp_log_writev(ESP_LOG_ERROR, tag, fmt, args);
    }
} // namespace

AstrOsLogger makeEspLogger()
{
    AstrOsLogger logger;
    logger.trace = &espLogVerbose;
    logger.debug = &espLogDebug;
    logger.info = &espLogInfo;
    logger.warn = &espLogWarn;
    logger.error = &espLogError;
    return logger;
}

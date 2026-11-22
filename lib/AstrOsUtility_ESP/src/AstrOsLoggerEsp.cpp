#include "AstrOsLoggerEsp.h"

#include <cstdio>
#include <esp_log.h>

namespace
{
    // esp_log_writev adds no level letter, timestamp, tag or newline
    // (esp_log_write.h), and the ESP_LOGx macros that do add them take varargs,
    // not a va_list. So each level formats into a LOG_LINE_MAX stack buffer and
    // logs it with ESP_LOGx(tag, "%s", buf); longer messages are truncated.
    // Verbose and debug are compiled out at CONFIG_LOG_MAXIMUM_LEVEL=3, so those
    // two return before formatting.
    constexpr size_t LOG_LINE_MAX = 256;

    void format(char (&buf)[LOG_LINE_MAX], const char *fmt, va_list args)
    {
        vsnprintf(buf, sizeof(buf), fmt, args);
    }

    void espLogVerbose(const char *tag, const char *fmt, va_list args)
    {
        if (LOG_LOCAL_LEVEL < ESP_LOG_VERBOSE)
        {
            return;
        }
        char buf[LOG_LINE_MAX];
        format(buf, fmt, args);
        ESP_LOGV(tag, "%s", buf);
    }

    void espLogDebug(const char *tag, const char *fmt, va_list args)
    {
        if (LOG_LOCAL_LEVEL < ESP_LOG_DEBUG)
        {
            return;
        }
        char buf[LOG_LINE_MAX];
        format(buf, fmt, args);
        ESP_LOGD(tag, "%s", buf);
    }

    void espLogInfo(const char *tag, const char *fmt, va_list args)
    {
        char buf[LOG_LINE_MAX];
        format(buf, fmt, args);
        ESP_LOGI(tag, "%s", buf);
    }

    void espLogWarn(const char *tag, const char *fmt, va_list args)
    {
        char buf[LOG_LINE_MAX];
        format(buf, fmt, args);
        ESP_LOGW(tag, "%s", buf);
    }

    void espLogError(const char *tag, const char *fmt, va_list args)
    {
        char buf[LOG_LINE_MAX];
        format(buf, fmt, args);
        ESP_LOGE(tag, "%s", buf);
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

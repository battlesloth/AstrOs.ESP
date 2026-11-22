#include "AstrOsLoggerEsp.h"

#include <cstdio>
#include <esp_log.h>

namespace
{
    // esp_log_writev takes a fully formatted line and adds no level, timestamp,
    // tag or newline, so format into a bounded buffer first, then log it through
    // the ESP_LOGx macros, which add the standard "W (ms) TAG: … \n" framing
    // (T-010 is the first caller). Longer messages are truncated.
    constexpr size_t LOG_LINE_MAX = 256;

    void format(char (&buf)[LOG_LINE_MAX], const char *fmt, va_list args)
    {
        vsnprintf(buf, sizeof(buf), fmt, args);
    }

    void espLogVerbose(const char *tag, const char *fmt, va_list args)
    {
        char buf[LOG_LINE_MAX];
        format(buf, fmt, args);
        ESP_LOGV(tag, "%s", buf);
    }

    void espLogDebug(const char *tag, const char *fmt, va_list args)
    {
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

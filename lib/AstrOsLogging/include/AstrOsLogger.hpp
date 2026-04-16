#pragma once

#include <cstdarg>
#include <cstddef>

// AstrOsLogger
// ------------
// A plain struct of function pointers used by PURE libs to emit diagnostics
// without pulling in ESP-IDF's logging headers. Default construction leaves
// every pointer null, in which case the astros_log_* helpers below become
// no-ops — this is the intended behaviour under [env:test].
//
// On-target code obtains a populated logger from
// lib/AstrOsUtility_ESP/include/AstrOsLoggerEsp.h (makeEspLogger), which
// wires each pointer to esp_log_writev() at the matching level.
//
// Signature matches esp_log_writev so the ESP adapter is a direct forward.
struct AstrOsLogger
{
    using LogFn = void (*)(const char *tag, const char *fmt, va_list args);

    LogFn trace = nullptr;
    LogFn debug = nullptr;
    LogFn info = nullptr;
    LogFn warn = nullptr;
    LogFn error = nullptr;
};

namespace astros_logging_detail
{
    inline void dispatch(AstrOsLogger::LogFn fn, const char *tag, const char *fmt, ...)
    {
        if (fn == nullptr)
        {
            return;
        }
        va_list args;
        va_start(args, fmt);
        fn(tag, fmt, args);
        va_end(args);
    }
} // namespace astros_logging_detail

// printf-style helpers. Safe to call with a default-constructed logger —
// a null function pointer short-circuits the call.
#define astros_log_trace(logger, tag, ...) ::astros_logging_detail::dispatch((logger).trace, (tag), __VA_ARGS__)
#define astros_log_debug(logger, tag, ...) ::astros_logging_detail::dispatch((logger).debug, (tag), __VA_ARGS__)
#define astros_log_info(logger, tag, ...) ::astros_logging_detail::dispatch((logger).info, (tag), __VA_ARGS__)
#define astros_log_warn(logger, tag, ...) ::astros_logging_detail::dispatch((logger).warn, (tag), __VA_ARGS__)
#define astros_log_error(logger, tag, ...) ::astros_logging_detail::dispatch((logger).error, (tag), __VA_ARGS__)

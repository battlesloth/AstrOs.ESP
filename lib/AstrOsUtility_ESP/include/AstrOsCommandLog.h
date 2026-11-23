#pragma once

// Per-command log lines (T-011): the dispatch loop's "… command val" lines, the
// modules' "Sending Command" (GPIO, I2C), "Queueing servo command" and
// "Setting servo" lines, and i2cQueueTask's "I2C Command received on queue".
// At INFO on the 115200 baud console they slow the dispatch loop and the queue
// tasks: on the 2026-11-22 bench (QA random-span-events.md case 11) the queue
// tasks took ~20 ms per GPIO and ~40 ms per servo message under load, ~21 ms of
// the latter being the two servo lines. So normal builds compile them out
// (DEBUG, above CONFIG_LOG_MAXIMUM_LEVEL), and bench builds log them at INFO:
//   PLATFORMIO_BUILD_FLAGS="-D ASTROS_VERBOSE_COMMANDS" pio run -e metro_s3 -t upload
// Keep each line's text unchanged: the bench QA plans match on it.
//
// ASTROS_VERBOSE_COMMANDS_ENABLED: main.cpp's boot line "Verbose command logs:
// on|off" reads it, so a bench console shows which build it is.

#include <esp_log.h>

#ifdef ASTROS_VERBOSE_COMMANDS
#define ASTROS_LOG_CMD(tag, format, ...) ESP_LOGI(tag, format, ##__VA_ARGS__)
constexpr bool ASTROS_VERBOSE_COMMANDS_ENABLED = true;
#else
#define ASTROS_LOG_CMD(tag, format, ...) ESP_LOGD(tag, format, ##__VA_ARGS__)
constexpr bool ASTROS_VERBOSE_COMMANDS_ENABLED = false;
#endif

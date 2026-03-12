#ifndef ASTROSFILEUTILS_H
#define ASTROSFILEUTILS_H

#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cctype>
#include <AstrOsStringUtils.hpp>
#include <AstrOsStructs.h>
#include <esp_log.h>

class AstrOsFileUtils
{
private:
    static constexpr const char *TAG = "AstrOsFileUtils";

    // Safe integer parsing without exceptions (ESP32-friendly)
    static bool safeStoi(const std::string &str, int &result, int defaultValue = 0)
    {
        if (str.empty())
        {
            result = defaultValue;
            return false;
        }

        // Check if string contains only valid characters
        const char *cstr = str.c_str();
        char *endptr = nullptr;

        // Reset errno before conversion
        errno = 0;
        long val = strtol(cstr, &endptr, 10);

        // Check for conversion errors
        if (endptr == cstr)
        {
            // No digits were found
            ESP_LOGE(TAG, "Invalid integer format: '%s', using default: %d", str.c_str(), defaultValue);
            result = defaultValue;
            return false;
        }

        if (errno == ERANGE || val > INT_MAX || val < INT_MIN)
        {
            // Out of range for int
            ESP_LOGE(TAG, "Integer out of range: '%s', using default: %d", str.c_str(), defaultValue);
            result = defaultValue;
            return false;
        }

        // Check if there are trailing non-whitespace characters
        if (*endptr != '\0')
        {
            // Find first non-whitespace trailing character
            while (*endptr && isspace(*endptr))
                endptr++;
            if (*endptr != '\0')
            {
                ESP_LOGW(TAG, "Partial parse of '%s', using parsed value %ld", str.c_str(), val);
            }
        }

        result = static_cast<int>(val);
        return true;
    }

public:
    static std::vector<maestro_config> parseMaestroConfig(const std::string maestroFile)
    {
        std::vector<maestro_config> configs;

        auto lines = AstrOsStringUtils::splitStringOnLineEnd(maestroFile);

        for (const auto &line : lines)
        {
            auto trimmedLine = line;
            // remove spaces
            trimmedLine.erase(std::remove(trimmedLine.begin(), trimmedLine.end(), ' '), trimmedLine.end());

            // skip empty lines and comments
            if (trimmedLine.empty() || trimmedLine[0] == '#')
            {
                continue;
            }

            auto parts = AstrOsStringUtils::splitString(trimmedLine, ':');
            if (parts.size() != 3) // must have 3 parts
            {
                continue;
            }

            maestro_config cfg;
            if (!safeStoi(parts[0], cfg.idx) ||
                !safeStoi(parts[1], cfg.uartChannel) ||
                !safeStoi(parts[2], cfg.baudrate))
            {
                ESP_LOGW(TAG, "Skipping invalid maestro config line: %s", trimmedLine.c_str());
                continue;
            }

            configs.push_back(cfg);
        }

        return configs;
    }

    static std::vector<servo_channel> parseServoConfig(const std::string servoFile)
    {
        auto cfigs = AstrOsStringUtils::splitString(servoFile, '|');

        // remove empty strings from the end of the vector
        if (!cfigs.empty() && cfigs.back().empty())
        {
            cfigs.pop_back();
        }

        //  get the max id from the configs
        int maxId = 0;
        for (const auto &cfig : cfigs)
        {
            auto parts = AstrOsStringUtils::splitString(cfig, ':');
            if (parts.size() != 7) // must have 7 parts
            {
                ESP_LOGW(TAG, "Skipping servo config with wrong field count: %s", cfig.c_str());
                continue;
            }

            // Check if any field is empty
            bool hasEmptyField = false;
            for (const auto &part : parts)
            {
                if (part.empty())
                {
                    hasEmptyField = true;
                    break;
                }
            }

            if (hasEmptyField)
            {
                ESP_LOGW(TAG, "Skipping servo config with empty field: %s", cfig.c_str());
                continue;
            }

            int id = 0;
            if (!safeStoi(parts[0], id))
            {
                ESP_LOGW(TAG, "Skipping servo config with invalid id: %s", cfig.c_str());
                continue;
            }

            if (id > maxId)
            {
                maxId = id;
            }
        }

        // Pre-allocate vector with default servo_channels
        std::vector<servo_channel> channels(maxId + 1);

        for (const auto &cfig : cfigs)
        {
            auto parts = AstrOsStringUtils::splitString(cfig, ':');
            if (parts.size() != 7) // must have 7 parts
            {
                continue;
            }

            // Check if any field is empty
            bool hasEmptyField = false;
            for (const auto &part : parts)
            {
                if (part.empty())
                {
                    hasEmptyField = true;
                    break;
                }
            }

            if (hasEmptyField)
            {
                continue;
            }

            servo_channel ch;
            int home = 0;
            int enabled = 0;
            int isServo = 0;
            int inverted = 0;

            if (!safeStoi(parts[0], ch.id) ||
                !safeStoi(parts[1], enabled) ||
                !safeStoi(parts[2], isServo) ||
                !safeStoi(parts[3], ch.minPos) ||
                !safeStoi(parts[4], ch.maxPos) ||
                !safeStoi(parts[5], home) ||
                !safeStoi(parts[6], inverted))
            {
                ESP_LOGW(TAG, "Skipping invalid servo config: %s", cfig.c_str());
                continue;
            }

            // Validate id is within bounds
            if (ch.id < 0 || ch.id > maxId)
            {
                ESP_LOGE(TAG, "Servo id %d out of range (0-%d)", ch.id, maxId);
                continue;
            }

            // Convert int values to bool
            ch.enabled = (enabled != 0);
            ch.isServo = (isServo != 0);
            ch.inverted = (inverted != 0);

            // make sure home is between min and max
            if (ch.inverted)
            {
                ch.home = ch.maxPos < home ? ch.maxPos : std::clamp(home, ch.minPos, ch.maxPos);
            }
            else
            {
                ch.home = ch.minPos > home ? ch.minPos : std::clamp(home, ch.minPos, ch.maxPos);
            }

            ch.currentPos = ch.minPos;
            ch.requestedPos = ch.home;
            ch.lastPos = ch.minPos;
            ch.speed = 0;
            ch.acceleration = 0;
            ch.on = false;

            // Assign to the correct position instead of inserting
            channels[ch.id] = ch;
        }

        return channels;
    }
};

#endif
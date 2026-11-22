#ifndef ASTROSFILEUTILS_H
#define ASTROSFILEUTILS_H

#include <AstrOsLogger.hpp>
#include <AstrOsServoUtils.hpp>
#include <AstrOsStringUtils.hpp>
#include <AstrOsStructs.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

class AstrOsFileUtils
{
private:
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
            result = defaultValue;
            return false;
        }

        if (errno == ERANGE || val > INT_MAX || val < INT_MIN)
        {
            // Out of range for int
            result = defaultValue;
            return false;
        }

        // Skip any trailing whitespace. Trailing non-whitespace is tolerated
        // (callers inspect the bool return; they do not need a partial-parse warning).
        if (*endptr != '\0')
        {
            while (*endptr && isspace(*endptr))
                endptr++;
        }

        result = static_cast<int>(val);
        return true;
    }

    // One Maestro channel entry (.docs/protocol.md § D):
    // id:enabled:isServo:minPos:maxPos:home:inverted[:releaseMs[:…]].
    // Fewer than 7 fields, or an empty one among the first 7, drops the entry.
    // Fields after the 8th are ignored (T-010), so a future field can't drop channels.
    // Trailing whitespace is trimmed (a hand-edited file's final newline). A
    // trailing ':' keeps its empty 8th field, which splitString would drop, so
    // "…:0:" (a server that lost releaseMs) warns instead of defaulting silently.
    static bool splitServoEntry(const std::string &entry, std::vector<std::string> &parts)
    {
        std::string trimmed = entry;
        while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back())))
        {
            trimmed.pop_back();
        }
        parts = AstrOsStringUtils::splitString(trimmed, ':');
        if (!trimmed.empty() && trimmed.back() == ':')
        {
            parts.push_back("");
        }
        if (parts.size() < 7)
        {
            return false;
        }
        for (size_t i = 0; i < 7; i++)
        {
            if (parts[i].empty())
            {
                return false;
            }
        }
        return true;
    }

    // The 8th field: a whole number of ms in MAESTRO_RELEASE_MIN_MS..MAX_MS,
    // digits only (safeStoi tolerates trailing characters; this must not).
    static bool parseReleaseMs(const std::string &field, int &result)
    {
        if (field.empty() || !std::all_of(field.begin(), field.end(), [](unsigned char c) { return std::isdigit(c); }))
        {
            return false;
        }
        int value = 0;
        if (!safeStoi(field, value) || value < MAESTRO_RELEASE_MIN_MS || value > MAESTRO_RELEASE_MAX_MS)
        {
            return false;
        }
        result = value;
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
            if (!safeStoi(parts[0], cfg.idx) || !safeStoi(parts[1], cfg.uartChannel) ||
                !safeStoi(parts[2], cfg.baudrate))
            {
                continue;
            }

            configs.push_back(cfg);
        }

        return configs;
    }

    static std::vector<servo_channel> parseServoConfig(const std::string servoFile,
                                                       const AstrOsLogger &logger = AstrOsLogger{})
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
            std::vector<std::string> parts;
            if (!splitServoEntry(cfig, parts))
            {
                continue;
            }

            int id = 0;
            if (!safeStoi(parts[0], id))
            {
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
            std::vector<std::string> parts;
            if (!splitServoEntry(cfig, parts))
            {
                continue;
            }

            servo_channel ch;
            int home = 0;
            int enabled = 0;
            int isServo = 0;
            int inverted = 0;

            if (!safeStoi(parts[0], ch.id) || !safeStoi(parts[1], enabled) || !safeStoi(parts[2], isServo) ||
                !safeStoi(parts[3], ch.minPos) || !safeStoi(parts[4], ch.maxPos) || !safeStoi(parts[5], home) ||
                !safeStoi(parts[6], inverted))
            {
                continue;
            }

            // Validate id is within bounds
            if (ch.id < 0 || ch.id > maxId)
            {
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

            // ch.releaseMs starts at the struct default; parseReleaseMs writes only on success.
            if (parts.size() >= 8 && !parseReleaseMs(parts[7], ch.releaseMs))
            {
                astros_log_warn(logger, "AstrOsFileUtils",
                                "Maestro channel %d: release time '%s' is not a whole number in %d-%d ms, using %d ms",
                                ch.id, parts[7].c_str(), MAESTRO_RELEASE_MIN_MS, MAESTRO_RELEASE_MAX_MS,
                                MAESTRO_RELEASE_DEFAULT_MS);
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

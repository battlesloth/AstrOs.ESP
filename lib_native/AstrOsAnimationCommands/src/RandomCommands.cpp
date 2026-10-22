#include "RandomCommands.hpp"

#include <AstrOsEnums.h>
#include <BaseCommand.hpp>

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <vector>

namespace
{
    constexpr size_t MAESTRO_RANDOM_FIELDS = 13;
    constexpr size_t GPIO_RANDOM_FIELDS = 6;

    constexpr int MIN_GAP_MS = 20;
    constexpr int MIN_SERVO_GAP_MS = 100;
    constexpr int MAX_POSITION = 100;
    constexpr int MAX_MAESTRO_MOTION = 255;

    // § C fields are plain base-10 integers: an optional '-' then digits. strtol
    // alone would skip leading whitespace, take a '+', and stop at an embedded NUL.
    bool ToInt(const std::string &field, int &out)
    {
        size_t firstDigit = (!field.empty() && field[0] == '-') ? 1 : 0;
        if (firstDigit >= field.size() || !std::isdigit(static_cast<unsigned char>(field[firstDigit])))
        {
            return false;
        }

        errno = 0;
        char *end = nullptr;
        long value = std::strtol(field.c_str(), &end, 10);

        // ERANGE only fires where long is 32-bit (the ESP32); on a 64-bit host the
        // int bounds catch the same overflow.
        if (end != field.c_str() + field.size() || errno == ERANGE || value < INT_MIN || value > INT_MAX)
        {
            return false;
        }

        out = static_cast<int>(value);
        return true;
    }

    // Splits val and converts every field. The type field (index 0) must equal
    // expectedType; a mismatch counts as FIELD_COUNT, i.e. "not this command".
    RandomParseError ParseFields(const std::string &val, size_t count, int expectedType, std::vector<int> &out)
    {
        BaseCommand splitter;
        str_vec_t parts = splitter.SplitTemplate(val);

        if (parts.size() != count)
        {
            return RandomParseError::FIELD_COUNT;
        }

        out.assign(count, 0);
        for (size_t i = 0; i < count; i++)
        {
            if (!ToInt(parts[i], out[i]))
            {
                return RandomParseError::NOT_A_NUMBER;
            }
        }

        if (out[0] != expectedType)
        {
            return RandomParseError::FIELD_COUNT;
        }

        return RandomParseError::NONE;
    }

    bool GapsValid(int spanMs, int minGapMs, int maxGapMs)
    {
        return minGapMs >= MIN_GAP_MS && maxGapMs >= minGapMs && spanMs >= minGapMs;
    }

    bool InMotionRange(int value)
    {
        return value >= 0 && value <= MAX_MAESTRO_MOTION;
    }
} // namespace

RandomMaestroParseResult ParseRandomMaestroCommand(const std::string &val)
{
    RandomMaestroParseResult result{RandomParseError::NONE, {}};

    std::vector<int> f;
    result.error = ParseFields(val, MAESTRO_RANDOM_FIELDS, MODULE_TYPE::MAESTRO_RANDOM, f);
    if (result.error != RandomParseError::NONE)
    {
        return result;
    }

    // f[0] type, f[1] delta (owned by the dispatch loop)
    result.cmd.module = f[2];
    result.cmd.channel = f[3];
    result.cmd.spanMs = f[4];
    result.cmd.minGapMs = f[5];
    result.cmd.maxGapMs = f[6];
    result.cmd.minPos = f[7];
    result.cmd.maxPos = f[8];
    result.cmd.minMove = f[9];
    result.cmd.maxMove = f[10];
    result.cmd.speed = f[11];
    result.cmd.acceleration = f[12];

    if (result.cmd.module < 0 || result.cmd.channel < 0 ||
        !GapsValid(result.cmd.spanMs, result.cmd.minGapMs, result.cmd.maxGapMs))
    {
        result.error = RandomParseError::OUT_OF_RANGE;
    }

    return result;
}

RandomGpioParseResult ParseRandomGpioCommand(const std::string &val)
{
    RandomGpioParseResult result{RandomParseError::NONE, {}};

    std::vector<int> f;
    result.error = ParseFields(val, GPIO_RANDOM_FIELDS, MODULE_TYPE::GPIO_RANDOM, f);
    if (result.error != RandomParseError::NONE)
    {
        return result;
    }

    // f[0] type, f[1] delta (owned by the dispatch loop)
    result.cmd.channel = f[2];
    result.cmd.spanMs = f[3];
    result.cmd.minGapMs = f[4];
    result.cmd.maxGapMs = f[5];

    if (result.cmd.channel < 0 || !GapsValid(result.cmd.spanMs, result.cmd.minGapMs, result.cmd.maxGapMs))
    {
        result.error = RandomParseError::OUT_OF_RANGE;
    }

    return result;
}

bool RandomServoFieldsValid(const RandomMaestroCommand &cmd)
{
    // Position bounds first, so the range arithmetic below cannot overflow.
    if (cmd.minPos < 0 || cmd.maxPos > MAX_POSITION || cmd.minPos >= cmd.maxPos)
    {
        return false;
    }

    int range = cmd.maxPos - cmd.minPos;

    // minMove <= range / 2 is 2 * minMove <= range for integers, without the overflow.
    bool moveValid = cmd.minMove >= 1 && cmd.minMove <= range / 2 &&
                     (cmd.maxMove == 0 || (cmd.maxMove >= cmd.minMove && cmd.maxMove <= range));

    return moveValid && cmd.minGapMs >= MIN_SERVO_GAP_MS && InMotionRange(cmd.speed) && InMotionRange(cmd.acceleration);
}

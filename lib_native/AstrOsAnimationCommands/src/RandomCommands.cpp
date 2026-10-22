#include "RandomCommands.hpp"

#include <AstrOsEnums.h>
#include <BaseCommand.hpp>

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <vector>

// No std::stoi here: exceptions are disabled, so a bad field would abort.

namespace
{
    constexpr size_t MAESTRO_RANDOM_FIELDS = 13;
    constexpr size_t GPIO_RANDOM_FIELDS = 6;

    constexpr int MIN_GAP_MS = 20;
    constexpr int MIN_SERVO_GAP_MS = 100;
    constexpr int MAX_POSITION = 100;
    constexpr int MAX_MAESTRO_MOTION = 255;

    // § C says base-10 integers; accept only an optional '-' then digits. strtol
    // alone would skip leading whitespace and take a '+'.
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

        // Compare against size(), not '\0': strtol stops at an embedded NUL.
        // Where long is 32-bit (ESP32, mingw-w64) strtol saturates an out-of-int
        // value to LONG_MIN/LONG_MAX, so ERANGE is the only check that rejects it;
        // where long is 64-bit (Linux, macOS, CI) the int bounds reject it first.
        if (end != field.c_str() + field.size() || errno == ERANGE || value < INT_MIN || value > INT_MAX)
        {
            return false;
        }

        out = static_cast<int>(value);
        return true;
    }

    // Splits val and converts every field. The type field (index 0) is checked
    // first: a mismatch is FIELD_COUNT ("not this command") whatever the other
    // fields hold.
    RandomParseError ParseFields(const std::string &val, size_t count, int expectedType, std::vector<int> &out)
    {
        BaseCommand splitter;
        str_vec_t parts = splitter.SplitTemplate(val);

        if (parts.size() != count)
        {
            return RandomParseError::FIELD_COUNT;
        }

        out.assign(count, 0);
        if (!ToInt(parts[0], out[0]))
        {
            return RandomParseError::NOT_A_NUMBER;
        }
        if (out[0] != expectedType)
        {
            return RandomParseError::FIELD_COUNT;
        }

        for (size_t i = 1; i < count; i++)
        {
            if (!ToInt(parts[i], out[i]))
            {
                return RandomParseError::NOT_A_NUMBER;
            }
        }

        return RandomParseError::NONE;
    }

    bool GapsValid(int spanMs, int minGapMs, int maxGapMs)
    {
        return minGapMs >= MIN_GAP_MS && maxGapMs >= minGapMs && spanMs >= minGapMs;
    }

    // The all-spans rules for a type 6 command: shared by the parser and the
    // servo check, so the servo check stands on its own.
    bool MaestroTargetAndGapsValid(const RandomMaestroCommand &cmd)
    {
        return cmd.module >= 0 && cmd.channel >= 0 && GapsValid(cmd.spanMs, cmd.minGapMs, cmd.maxGapMs);
    }

    bool InMotionRange(int value)
    {
        return value >= 0 && value <= MAX_MAESTRO_MOTION;
    }

    RandomMaestroParseResult MaestroReject(RandomParseError error)
    {
        RandomMaestroParseResult result{error, {}};
        result.cmd.module = -1;
        result.cmd.channel = -1;
        return result;
    }

    RandomGpioParseResult GpioReject(RandomParseError error)
    {
        RandomGpioParseResult result{error, {}};
        result.cmd.channel = -1;
        return result;
    }
} // namespace

RandomMaestroParseResult ParseRandomMaestroCommand(const std::string &val)
{
    std::vector<int> f;
    RandomParseError error = ParseFields(val, MAESTRO_RANDOM_FIELDS, MODULE_TYPE::MAESTRO_RANDOM, f);
    if (error != RandomParseError::NONE)
    {
        return MaestroReject(error);
    }

    // f[0] is the type and f[1] the delta: both must be ints, but the delta isn't
    // stored (the dispatch loop already used it).
    RandomMaestroCommand cmd{};
    cmd.module = f[2];
    cmd.channel = f[3];
    cmd.spanMs = f[4];
    cmd.minGapMs = f[5];
    cmd.maxGapMs = f[6];
    cmd.minPos = f[7];
    cmd.maxPos = f[8];
    cmd.minMove = f[9];
    cmd.maxMove = f[10];
    cmd.speed = f[11];
    cmd.acceleration = f[12];

    if (!MaestroTargetAndGapsValid(cmd))
    {
        return MaestroReject(RandomParseError::OUT_OF_RANGE);
    }

    return {RandomParseError::NONE, cmd};
}

RandomGpioParseResult ParseRandomGpioCommand(const std::string &val)
{
    std::vector<int> f;
    RandomParseError error = ParseFields(val, GPIO_RANDOM_FIELDS, MODULE_TYPE::GPIO_RANDOM, f);
    if (error != RandomParseError::NONE)
    {
        return GpioReject(error);
    }

    // f[0] is the type and f[1] the delta (see ParseRandomMaestroCommand).
    RandomGpioCommand cmd{};
    cmd.channel = f[2];
    cmd.spanMs = f[3];
    cmd.minGapMs = f[4];
    cmd.maxGapMs = f[5];

    if (cmd.channel < 0 || !GapsValid(cmd.spanMs, cmd.minGapMs, cmd.maxGapMs))
    {
        return GpioReject(RandomParseError::OUT_OF_RANGE);
    }

    return {RandomParseError::NONE, cmd};
}

bool RandomServoFieldsValid(const RandomMaestroCommand &cmd)
{
    if (!MaestroTargetAndGapsValid(cmd))
    {
        return false;
    }

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

const char *RandomParseErrorName(RandomParseError error)
{
    // No default, so -Wswitch flags a new enumerator that has no name yet.
    switch (error)
    {
    case RandomParseError::NONE:
        return "NONE";
    case RandomParseError::FIELD_COUNT:
        return "FIELD_COUNT";
    case RandomParseError::NOT_A_NUMBER:
        return "NOT_A_NUMBER";
    case RandomParseError::OUT_OF_RANGE:
        return "OUT_OF_RANGE";
    }
    return "UNKNOWN";
}

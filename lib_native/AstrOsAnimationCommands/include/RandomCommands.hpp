#ifndef RANDOMCOMMANDS_HPP
#define RANDOMCOMMANDS_HPP

#include <string>

// Parsers for the random span script commands (types 6 and 7). Wire format and
// valid ranges: .docs/protocol.md § C. Free functions with rich results so the
// MIXED caller logs at the boundary; no std::stoi (exceptions are disabled).

enum class RandomParseError
{
    NONE,
    FIELD_COUNT,  // wrong number of fields, or the type field is not 6 / 7
    NOT_A_NUMBER, // a field is empty, non-numeric, or outside int
    OUT_OF_RANGE  // a negative module/channel, or a gap/span rule is broken
};

// 6|delta|maestroIdx|channel|spanMs|minGapMs|maxGapMs|minPos|maxPos|minMove|maxMove|speed|accel
struct RandomMaestroCommand
{
    int module;
    int channel;
    int spanMs;
    int minGapMs;
    int maxGapMs;
    int minPos;
    int maxPos;
    int minMove;
    int maxMove; // 0 = no max
    int speed;
    int acceleration;
};

// 7|delta|gpioChannel|spanMs|minGapMs|maxGapMs
struct RandomGpioCommand
{
    int channel;
    int spanMs;
    int minGapMs;
    int maxGapMs;
};

struct RandomMaestroParseResult
{
    RandomParseError error;
    RandomMaestroCommand cmd;
};

struct RandomGpioParseResult
{
    RandomParseError error;
    RandomGpioCommand cmd;
};

RandomMaestroParseResult ParseRandomMaestroCommand(const std::string &val);
RandomGpioParseResult ParseRandomGpioCommand(const std::string &val);

// Servo-only rules. Call once the channel is known to be a servo; a digital
// Maestro channel's zeroed fields never pass.
bool RandomServoFieldsValid(const RandomMaestroCommand &cmd);

#endif

#ifndef RANDOMCOMMANDS_HPP
#define RANDOMCOMMANDS_HPP

#include <string>

// Parsers for the random span script commands (types 6 and 7). Wire format and
// valid ranges: .docs/protocol.md § C. Free functions with rich results so the
// MIXED caller logs at the boundary.

enum class RandomParseError
{
    NONE,
    FIELD_COUNT,  // wrong number of fields, or the type field isn't this parser's (6 Maestro, 7 GPIO)
    NOT_A_NUMBER, // a field is empty, not an optional '-' plus digits, or outside int
    OUT_OF_RANGE  // a negative module/channel, or an all-spans gap/span rule (§ C) is broken
};

// 6|delta|maestroIdx|channel|spanMs|minGapMs|maxGapMs|minPos|maxPos|minMove|maxMove|speed|accel
//
// A NONE parse guarantees module/channel >= 0 and the all-spans gap rules.
// minPos..acceleration are checked only by RandomServoFieldsValid; on a digital
// channel the server sends them as 0 and the firmware ignores them.
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
    int maxMove; // 0 = no max (the same as maxPos - minPos)
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

// cmd is meaningful only when error == NONE. On any error cmd is zeroed with
// module and channel set to -1, so it names no real target; log the raw command.
struct [[nodiscard]] RandomMaestroParseResult
{
    RandomParseError error;
    RandomMaestroCommand cmd;
};

struct [[nodiscard]] RandomGpioParseResult
{
    RandomParseError error;
    RandomGpioCommand cmd;
};

RandomMaestroParseResult ParseRandomMaestroCommand(const std::string &val);
RandomGpioParseResult ParseRandomGpioCommand(const std::string &val);

// True when every § C rule for a servo channel holds: the parser's rules plus the
// servo-only ones. Cannot tell servo from digital (isServo comes from channel
// config); a digital channel's zeroed fields never pass.
[[nodiscard]] bool RandomServoFieldsValid(const RandomMaestroCommand &cmd);

// For log lines: "NONE", "FIELD_COUNT", "NOT_A_NUMBER", "OUT_OF_RANGE", or "UNKNOWN".
[[nodiscard]] const char *RandomParseErrorName(RandomParseError error);

#endif

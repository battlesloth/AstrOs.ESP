#include <AnimationCommand.hpp>
#include <AstrOsAnimationEngine.hpp>
#include <AstrOsEnums.h>
#include <RandomCommands.hpp>
#include <gtest/gtest.h>

#include <climits>
#include <cstring>
#include <set>
#include <string>

// Wire format: .docs/protocol.md § C (types 6 and 7).

namespace
{
    RandomMaestroCommand ValidServo()
    {
        RandomMaestroCommand cmd{};
        cmd.module = 0;
        cmd.channel = 3;
        cmd.spanMs = 14000;
        cmd.minGapMs = 400;
        cmd.maxGapMs = 2000;
        cmd.minPos = 20;
        cmd.maxPos = 100;
        cmd.minMove = 15;
        cmd.maxMove = 60;
        cmd.speed = 0;
        cmd.acceleration = 0;
        return cmd;
    }
} // namespace

// ---------------- ParseRandomMaestroCommand / ParseRandomGpioCommand ----------------

TEST(RandomCommands, ParsesValidMaestroRandom)
{
    // Every field distinct, so a swapped field index shows up as a wrong value.
    auto result = ParseRandomMaestroCommand("6|1500|1|3|14000|400|2000|20|100|15|60|7|9");
    ASSERT_EQ(RandomParseError::NONE, result.error);
    EXPECT_EQ(1, result.cmd.module);
    EXPECT_EQ(3, result.cmd.channel);
    EXPECT_EQ(14000, result.cmd.spanMs);
    EXPECT_EQ(400, result.cmd.minGapMs);
    EXPECT_EQ(2000, result.cmd.maxGapMs);
    EXPECT_EQ(20, result.cmd.minPos);
    EXPECT_EQ(100, result.cmd.maxPos);
    EXPECT_EQ(15, result.cmd.minMove);
    EXPECT_EQ(60, result.cmd.maxMove);
    EXPECT_EQ(7, result.cmd.speed);
    EXPECT_EQ(9, result.cmd.acceleration);
}

TEST(RandomCommands, ParsesValidMaestroRandomNonZeroMotion)
{
    auto result = ParseRandomMaestroCommand("6|0|1|23|5000|100|300|35|65|2|8|40|7");
    ASSERT_EQ(RandomParseError::NONE, result.error);
    EXPECT_EQ(1, result.cmd.module);
    EXPECT_EQ(23, result.cmd.channel);
    EXPECT_EQ(40, result.cmd.speed);
    EXPECT_EQ(7, result.cmd.acceleration);
}

TEST(RandomCommands, ParsesValidGpioRandom)
{
    // Every field distinct, so a swapped field index shows up as a wrong value.
    auto result = ParseRandomGpioCommand("7|1500|2|14000|50|400");
    ASSERT_EQ(RandomParseError::NONE, result.error);
    EXPECT_EQ(2, result.cmd.channel);
    EXPECT_EQ(14000, result.cmd.spanMs);
    EXPECT_EQ(50, result.cmd.minGapMs);
    EXPECT_EQ(400, result.cmd.maxGapMs);
}

TEST(RandomCommands, MaxMoveZeroMeansNoMax)
{
    auto result = ParseRandomMaestroCommand("6|0|0|3|14000|400|2000|20|100|15|0|0|0");
    ASSERT_EQ(RandomParseError::NONE, result.error);
    EXPECT_EQ(0, result.cmd.maxMove);
    EXPECT_TRUE(RandomServoFieldsValid(result.cmd));
}

TEST(RandomCommands, RejectsWrongFieldCount)
{
    // 12 and 14 fields for type 6
    EXPECT_EQ(RandomParseError::FIELD_COUNT, ParseRandomMaestroCommand("6|0|0|3|14000|400|2000|20|100|15|60|0").error);
    EXPECT_EQ(RandomParseError::FIELD_COUNT,
              ParseRandomMaestroCommand("6|0|0|3|14000|400|2000|20|100|15|60|0|0|0").error);
    // 5 and 7 fields for type 7
    EXPECT_EQ(RandomParseError::FIELD_COUNT, ParseRandomGpioCommand("7|0|2|14000|50").error);
    EXPECT_EQ(RandomParseError::FIELD_COUNT, ParseRandomGpioCommand("7|0|2|14000|50|400|0").error);
    EXPECT_EQ(RandomParseError::FIELD_COUNT, ParseRandomGpioCommand("").error);
}

TEST(RandomCommands, RejectsWrongTypeField)
{
    // Right field count, wrong command type.
    EXPECT_EQ(RandomParseError::FIELD_COUNT,
              ParseRandomMaestroCommand("1|0|0|3|14000|400|2000|20|100|15|60|0|0").error);
    EXPECT_EQ(RandomParseError::FIELD_COUNT, ParseRandomGpioCommand("5|0|2|14000|50|400").error);
    // The type is checked before the other fields, so a wrong type wins over a
    // bad number elsewhere: the command is "not this command", whatever else.
    EXPECT_EQ(RandomParseError::FIELD_COUNT, ParseRandomMaestroCommand("1|0|0|3|abc|400|2000|20|100|15|60|0|0").error);
    EXPECT_EQ(RandomParseError::FIELD_COUNT, ParseRandomGpioCommand("5|0|2|x|50|400").error);
    // A non-numeric type field is still NOT_A_NUMBER.
    EXPECT_EQ(RandomParseError::NOT_A_NUMBER, ParseRandomGpioCommand("x|0|2|14000|50|400").error);
    // A type 7 string handed to the type 6 parser (and the reverse) is rejected on
    // field count before the type is read.
    EXPECT_EQ(RandomParseError::FIELD_COUNT, ParseRandomMaestroCommand("7|14000|2|14000|50|400").error);
    EXPECT_EQ(RandomParseError::FIELD_COUNT,
              ParseRandomGpioCommand("6|14000|0|3|14000|400|2000|20|100|15|60|0|0").error);
}

TEST(RandomCommands, RejectsNonNumeric)
{
    EXPECT_EQ(RandomParseError::NOT_A_NUMBER,
              ParseRandomMaestroCommand("6|0|0|abc|14000|400|2000|20|100|15|60|0|0").error);
    EXPECT_EQ(RandomParseError::NOT_A_NUMBER,
              ParseRandomMaestroCommand("6|0|0|3|14000|12x|2000|20|100|15|60|0|0").error);
    EXPECT_EQ(RandomParseError::NOT_A_NUMBER, ParseRandomMaestroCommand("6|0|0|3||400|2000|20|100|15|60|0|0").error);
    EXPECT_EQ(RandomParseError::NOT_A_NUMBER,
              ParseRandomMaestroCommand("6|0|0|3|99999999999|400|2000|20|100|15|60|0|0").error);
    EXPECT_EQ(RandomParseError::NOT_A_NUMBER,
              ParseRandomMaestroCommand("6|0|0|3|-99999999999|400|2000|20|100|15|60|0|0").error);
    EXPECT_EQ(RandomParseError::NOT_A_NUMBER, ParseRandomGpioCommand("7|0|2|14000|50|4o0").error);
    EXPECT_EQ(RandomParseError::NOT_A_NUMBER, ParseRandomGpioCommand("7|x|2|14000|50|400").error);
}

TEST(RandomCommands, RejectsLooseNumbers)
{
    // Only an optional '-' then digits; strtol alone is more lenient.
    EXPECT_EQ(RandomParseError::NOT_A_NUMBER, ParseRandomGpioCommand("7|0|2|14000| 50|400").error);
    EXPECT_EQ(RandomParseError::NOT_A_NUMBER, ParseRandomGpioCommand("7|0|2|14000|+50|400").error);
    EXPECT_EQ(RandomParseError::NOT_A_NUMBER, ParseRandomGpioCommand("7|0|2|14000|50 |400").error);
    EXPECT_EQ(RandomParseError::NOT_A_NUMBER, ParseRandomGpioCommand("7|0|2|14000|-|400").error);

    std::string withNul = "7|0|2|14000|50";
    withNul.push_back('\0');
    withNul += "9|400";
    EXPECT_EQ(RandomParseError::NOT_A_NUMBER, ParseRandomGpioCommand(withNul).error);
}

TEST(RandomCommands, RejectsOutOfRangeFields)
{
    // minGapMs below 20
    EXPECT_EQ(RandomParseError::OUT_OF_RANGE, ParseRandomGpioCommand("7|0|2|14000|19|400").error);
    EXPECT_EQ(RandomParseError::OUT_OF_RANGE, ParseRandomMaestroCommand("6|0|0|3|14000|19|2000|0|0|0|0|0|0").error);
    // maxGapMs below minGapMs
    EXPECT_EQ(RandomParseError::OUT_OF_RANGE, ParseRandomGpioCommand("7|0|2|14000|400|50").error);
    EXPECT_EQ(RandomParseError::OUT_OF_RANGE,
              ParseRandomMaestroCommand("6|0|0|3|14000|400|300|20|100|15|60|0|0").error);
    // spanMs below minGapMs
    EXPECT_EQ(RandomParseError::OUT_OF_RANGE, ParseRandomGpioCommand("7|0|2|40|50|400").error);
    EXPECT_EQ(RandomParseError::OUT_OF_RANGE, ParseRandomMaestroCommand("6|0|0|3|300|400|2000|20|100|15|60|0|0").error);
    // negative channel / module
    EXPECT_EQ(RandomParseError::OUT_OF_RANGE, ParseRandomGpioCommand("7|0|-1|14000|50|400").error);
    EXPECT_EQ(RandomParseError::OUT_OF_RANGE,
              ParseRandomMaestroCommand("6|0|0|-3|14000|400|2000|20|100|15|60|0|0").error);
    EXPECT_EQ(RandomParseError::OUT_OF_RANGE,
              ParseRandomMaestroCommand("6|0|-1|3|14000|400|2000|20|100|15|60|0|0").error);
}

TEST(RandomCommands, AcceptsGapBoundaries)
{
    // minGapMs == 20, maxGapMs == minGapMs, spanMs == minGapMs
    auto gpio = ParseRandomGpioCommand("7|0|0|20|20|20");
    EXPECT_EQ(RandomParseError::NONE, gpio.error);
    EXPECT_EQ(0, gpio.cmd.channel);

    // The same boundaries on a digital Maestro channel (zeroed servo fields).
    auto maestro = ParseRandomMaestroCommand("6|0|0|3|20|20|20|0|0|0|0|0|0");
    EXPECT_EQ(RandomParseError::NONE, maestro.error);
}

TEST(RandomCommands, AcceptsSpanShorterThanMaxGap)
{
    // § C only asks spanMs >= minGapMs; a span may end before a max gap could.
    EXPECT_EQ(RandomParseError::NONE, ParseRandomGpioCommand("7|0|2|300|50|400").error);
    EXPECT_EQ(RandomParseError::NONE, ParseRandomMaestroCommand("6|0|0|3|1000|400|2000|20|100|15|60|0|0").error);
}

TEST(RandomCommands, AcceptsChannelAndModuleZero)
{
    // Module 0 and channel 0 are the first on every board.
    auto result = ParseRandomMaestroCommand("6|0|0|0|14000|400|2000|0|100|15|60|0|0");
    ASSERT_EQ(RandomParseError::NONE, result.error);
    EXPECT_EQ(0, result.cmd.module);
    EXPECT_EQ(0, result.cmd.channel);
    EXPECT_TRUE(RandomServoFieldsValid(result.cmd));
}

TEST(RandomCommands, ErrorResultsCarryNoTarget)
{
    // On any error the command names no real channel, so a caller that forgets to
    // check error cannot drive module 0 / channel 0.
    const char *badMaestro[] = {
        "6|0|0|3|14000|400|2000|20|100|15|60|0",     // FIELD_COUNT
        "6|0|0|abc|14000|400|2000|20|100|15|60|0|0", // NOT_A_NUMBER
        "6|0|0|3|300|400|2000|20|100|15|60|0|0",     // OUT_OF_RANGE
    };
    for (const char *val : badMaestro)
    {
        auto result = ParseRandomMaestroCommand(val);
        ASSERT_NE(RandomParseError::NONE, result.error) << val;
        EXPECT_EQ(-1, result.cmd.module) << val;
        EXPECT_EQ(-1, result.cmd.channel) << val;
    }

    const char *badGpio[] = {
        "7|0|2|14000|50",     // FIELD_COUNT
        "7|0|2|14000|50|4o0", // NOT_A_NUMBER
        "7|0|2|40|50|400",    // OUT_OF_RANGE
    };
    for (const char *val : badGpio)
    {
        auto result = ParseRandomGpioCommand(val);
        ASSERT_NE(RandomParseError::NONE, result.error) << val;
        EXPECT_EQ(-1, result.cmd.channel) << val;
    }
}

// ---------------- RandomServoFieldsValid ----------------

TEST(RandomCommands, ServoFieldBoundaries)
{
    EXPECT_TRUE(RandomServoFieldsValid(ValidServo()));

    auto cmd = ValidServo();
    cmd.minMove = 40; // exactly half of the 20-100 range
    EXPECT_TRUE(RandomServoFieldsValid(cmd));
    cmd.minMove = 41;
    EXPECT_FALSE(RandomServoFieldsValid(cmd));

    cmd = ValidServo();
    cmd.minPos = 19; // odd range of 81: half rounds down to 40
    cmd.minMove = 40;
    EXPECT_TRUE(RandomServoFieldsValid(cmd));
    cmd.minMove = 41;
    EXPECT_FALSE(RandomServoFieldsValid(cmd));

    cmd = ValidServo();
    cmd.minMove = 1; // the smallest allowed move
    EXPECT_TRUE(RandomServoFieldsValid(cmd));

    cmd = ValidServo();
    cmd.maxMove = cmd.minMove; // a fixed move size
    EXPECT_TRUE(RandomServoFieldsValid(cmd));

    cmd = ValidServo();
    cmd.maxMove = 80; // the whole range
    EXPECT_TRUE(RandomServoFieldsValid(cmd));
    cmd.maxMove = 81;
    EXPECT_FALSE(RandomServoFieldsValid(cmd));

    cmd = ValidServo();
    cmd.maxMove = 10; // below minMove 15
    EXPECT_FALSE(RandomServoFieldsValid(cmd));

    cmd = ValidServo();
    cmd.maxMove = -1; // only 0 means "no max"
    EXPECT_FALSE(RandomServoFieldsValid(cmd));

    cmd = ValidServo();
    cmd.minGapMs = 100;
    EXPECT_TRUE(RandomServoFieldsValid(cmd));
    cmd.minGapMs = 99;
    EXPECT_FALSE(RandomServoFieldsValid(cmd));

    cmd = ValidServo();
    cmd.speed = 255;
    cmd.acceleration = 255;
    EXPECT_TRUE(RandomServoFieldsValid(cmd));
    cmd.speed = 256;
    EXPECT_FALSE(RandomServoFieldsValid(cmd));
    cmd.speed = -1;
    EXPECT_FALSE(RandomServoFieldsValid(cmd));
    cmd.speed = 0;
    cmd.acceleration = 256;
    EXPECT_FALSE(RandomServoFieldsValid(cmd));
    cmd.acceleration = -1;
    EXPECT_FALSE(RandomServoFieldsValid(cmd));

    cmd = ValidServo();
    cmd.minPos = 50;
    cmd.maxPos = 50;
    EXPECT_FALSE(RandomServoFieldsValid(cmd));

    cmd = ValidServo();
    cmd.minPos = 0; // the full range, e.g. a panel flap
    cmd.maxPos = 100;
    EXPECT_TRUE(RandomServoFieldsValid(cmd));

    cmd = ValidServo();
    cmd.minPos = -1;
    EXPECT_FALSE(RandomServoFieldsValid(cmd));

    cmd = ValidServo();
    cmd.maxPos = 101;
    EXPECT_FALSE(RandomServoFieldsValid(cmd));

    cmd = ValidServo();
    cmd.minMove = 0;
    EXPECT_FALSE(RandomServoFieldsValid(cmd));
}

TEST(RandomCommands, ServoCheckRejectsExtremePositions)
{
    // INT_MIN is a legal field, so maxPos - minPos could overflow; the position
    // bounds must reject it before any range arithmetic.
    auto result = ParseRandomMaestroCommand("6|0|0|3|14000|400|2000|1|-2147483648|15|0|0|0");
    ASSERT_EQ(RandomParseError::NONE, result.error);
    EXPECT_FALSE(RandomServoFieldsValid(result.cmd));

    auto cmd = ValidServo();
    cmd.minPos = INT_MAX;
    EXPECT_FALSE(RandomServoFieldsValid(cmd));
}

TEST(RandomCommands, ServoCheckIsSelfContained)
{
    // A true result means every § C rule for a servo channel holds, including the
    // ones the parser checks, wherever the command came from.
    auto cmd = ValidServo();
    cmd.spanMs = 300; // below minGapMs 400
    EXPECT_FALSE(RandomServoFieldsValid(cmd));

    cmd = ValidServo();
    cmd.maxGapMs = 300; // below minGapMs 400
    EXPECT_FALSE(RandomServoFieldsValid(cmd));

    cmd = ValidServo();
    cmd.channel = -1;
    EXPECT_FALSE(RandomServoFieldsValid(cmd));

    cmd = ValidServo();
    cmd.module = -1;
    EXPECT_FALSE(RandomServoFieldsValid(cmd));
}

TEST(RandomCommands, DigitalZerosParseButFailServoCheck)
{
    // A digital Maestro channel gets zeros after maxGapMs. They parse, and fail the
    // servo check, which only runs on servo channels.
    auto result = ParseRandomMaestroCommand("6|0|0|3|5000|50|200|0|0|0|0|0|0");
    ASSERT_EQ(RandomParseError::NONE, result.error);
    EXPECT_FALSE(RandomServoFieldsValid(result.cmd));
}

// ---------------- RandomParseErrorName ----------------

TEST(RandomCommands, ParseErrorNamesAreDistinct)
{
    const RandomParseError errors[] = {RandomParseError::NONE, RandomParseError::FIELD_COUNT,
                                       RandomParseError::NOT_A_NUMBER, RandomParseError::OUT_OF_RANGE};
    std::set<std::string> names;
    for (RandomParseError error : errors)
    {
        const char *name = RandomParseErrorName(error);
        ASSERT_NE(nullptr, name);
        EXPECT_GT(std::strlen(name), 0u);
        names.insert(name);
    }
    EXPECT_EQ(4u, names.size());

    const char *unknown = RandomParseErrorName(static_cast<RandomParseError>(255));
    ASSERT_NE(nullptr, unknown);
    EXPECT_EQ(0u, names.count(unknown));
}

// ---------------- AnimationCommand header parse and engine hand-off ----------------

TEST(RandomCommands, AnimationCommandTypesNewValues)
{
    // delta differs from spanMs, so duration is shown to be the delta.
    AnimationCommand maestro("6|1500|0|3|14000|400|2000|20|100|15|60|0|0");
    EXPECT_EQ(MODULE_TYPE::MAESTRO_RANDOM, maestro.commandType);
    EXPECT_EQ(1500, maestro.duration);
    EXPECT_EQ(0, maestro.module);

    AnimationCommand gpio("7|250|2|9000|50|400");
    EXPECT_EQ(MODULE_TYPE::GPIO_RANDOM, gpio.commandType);
    EXPECT_EQ(250, gpio.duration);
    EXPECT_EQ(2, gpio.module);
}

TEST(RandomCommands, EngineHandsOffRandomCommandsWithDelta)
{
    // The dispatch loop waits the delta, not the span, and hands the whole command
    // string (type field included) to whoever parses it.
    auto events =
        AstrOsAnimationEngine::parseAnimationScript("6|1500|1|3|14000|400|2000|20|100|15|60|7|9;7|250|2|9000|50|400");

    auto first = AstrOsAnimationEngine::getNextCommand(events);
    ASSERT_NE(nullptr, first.command);
    EXPECT_EQ(MODULE_TYPE::MAESTRO_RANDOM, first.command->type);
    EXPECT_EQ(1500, first.delayMs);
    EXPECT_EQ(RandomParseError::NONE, ParseRandomMaestroCommand(first.command->val).error);

    auto second = AstrOsAnimationEngine::getNextCommand(events);
    ASSERT_NE(nullptr, second.command);
    EXPECT_EQ(MODULE_TYPE::GPIO_RANDOM, second.command->type);
    EXPECT_EQ(250, second.delayMs);
    EXPECT_EQ(RandomParseError::NONE, ParseRandomGpioCommand(second.command->val).error);
}

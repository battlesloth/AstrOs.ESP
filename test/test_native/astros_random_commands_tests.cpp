#include <AnimationCommand.hpp>
#include <AstrOsEnums.h>
#include <RandomCommands.hpp>
#include <gtest/gtest.h>

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
    auto result = ParseRandomMaestroCommand("6|14000|0|3|14000|400|2000|20|100|15|60|0|0");
    ASSERT_EQ(RandomParseError::NONE, result.error);
    EXPECT_EQ(0, result.cmd.module);
    EXPECT_EQ(3, result.cmd.channel);
    EXPECT_EQ(14000, result.cmd.spanMs);
    EXPECT_EQ(400, result.cmd.minGapMs);
    EXPECT_EQ(2000, result.cmd.maxGapMs);
    EXPECT_EQ(20, result.cmd.minPos);
    EXPECT_EQ(100, result.cmd.maxPos);
    EXPECT_EQ(15, result.cmd.minMove);
    EXPECT_EQ(60, result.cmd.maxMove);
    EXPECT_EQ(0, result.cmd.speed);
    EXPECT_EQ(0, result.cmd.acceleration);
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
    auto result = ParseRandomGpioCommand("7|14000|2|14000|50|400");
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
    // A type 7 string handed to the type 6 parser (and vice versa) never parses.
    EXPECT_NE(RandomParseError::NONE, ParseRandomMaestroCommand("7|14000|2|14000|50|400").error);
    EXPECT_NE(RandomParseError::NONE, ParseRandomGpioCommand("6|14000|0|3|14000|400|2000|20|100|15|60|0|0").error);
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
    EXPECT_EQ(RandomParseError::NOT_A_NUMBER, ParseRandomGpioCommand("7|0|2|14000|50|4o0").error);
    EXPECT_EQ(RandomParseError::NOT_A_NUMBER, ParseRandomGpioCommand("7|x|2|14000|50|400").error);
}

TEST(RandomCommands, RejectsGapAndSpanRules)
{
    // minGapMs below 20
    EXPECT_EQ(RandomParseError::OUT_OF_RANGE, ParseRandomGpioCommand("7|0|2|14000|19|400").error);
    EXPECT_EQ(RandomParseError::OUT_OF_RANGE, ParseRandomMaestroCommand("6|0|0|3|14000|19|2000|0|0|0|0|0|0").error);
    // maxGapMs below minGapMs
    EXPECT_EQ(RandomParseError::OUT_OF_RANGE, ParseRandomGpioCommand("7|0|2|14000|400|50").error);
    // spanMs below minGapMs
    EXPECT_EQ(RandomParseError::OUT_OF_RANGE, ParseRandomGpioCommand("7|0|2|40|50|400").error);
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
    auto result = ParseRandomGpioCommand("7|0|0|20|20|20");
    EXPECT_EQ(RandomParseError::NONE, result.error);
    EXPECT_EQ(0, result.cmd.channel);
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
    cmd.maxMove = 80; // the whole range
    EXPECT_TRUE(RandomServoFieldsValid(cmd));
    cmd.maxMove = 81;
    EXPECT_FALSE(RandomServoFieldsValid(cmd));

    cmd = ValidServo();
    cmd.maxMove = 10; // below minMove 15
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

TEST(RandomCommands, DigitalZerosAreParseable)
{
    // A digital Maestro channel gets zeros after maxGapMs. They parse. T-009
    // never runs RandomServoFieldsValid on a digital channel, so failing it
    // here is expected and harmless.
    auto result = ParseRandomMaestroCommand("6|0|0|3|5000|50|200|0|0|0|0|0|0");
    ASSERT_EQ(RandomParseError::NONE, result.error);
    EXPECT_FALSE(RandomServoFieldsValid(result.cmd));
}

// ---------------- AnimationCommand header parse ----------------

TEST(RandomCommands, AnimationCommandTypesNewValues)
{
    AnimationCommand maestro("6|14000|0|3|14000|400|2000|20|100|15|60|0|0");
    EXPECT_EQ(MODULE_TYPE::MAESTRO_RANDOM, maestro.commandType);
    EXPECT_EQ(14000, maestro.duration);
    EXPECT_EQ(0, maestro.module);

    AnimationCommand gpio("7|14000|2|14000|50|400");
    EXPECT_EQ(MODULE_TYPE::GPIO_RANDOM, gpio.commandType);
    EXPECT_EQ(14000, gpio.duration);
    EXPECT_EQ(2, gpio.module);
}

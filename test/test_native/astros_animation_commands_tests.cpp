#include <AnimationCommand.hpp>
#include <AnimationCommon.hpp>
#include <AstrOsEnums.h>
#include <BaseCommand.hpp>
#include <GpioCommand.hpp>
#include <I2cCommand.hpp>
#include <MaestroCommand.hpp>
#include <SerialCommand.hpp>
#include <gtest/gtest.h>

#include <string>

// ---------------- BaseCommand::SplitTemplate ----------------

TEST(AnimationCommands, SplitTemplateBasic)
{
    BaseCommand base;
    auto parts = base.SplitTemplate("1|500|3|hello|world");
    ASSERT_EQ(5u, parts.size());
    EXPECT_EQ("1", parts[0]);
    EXPECT_EQ("500", parts[1]);
    EXPECT_EQ("3", parts[2]);
    EXPECT_EQ("hello", parts[3]);
    EXPECT_EQ("world", parts[4]);
}

TEST(AnimationCommands, SplitTemplateSingleField)
{
    BaseCommand base;
    auto parts = base.SplitTemplate("only");
    ASSERT_EQ(1u, parts.size());
    EXPECT_EQ("only", parts[0]);
}

TEST(AnimationCommands, SplitTemplateEmptyFields)
{
    BaseCommand base;
    auto parts = base.SplitTemplate("a||b");
    ASSERT_EQ(3u, parts.size());
    EXPECT_EQ("a", parts[0]);
    EXPECT_EQ("", parts[1]);
    EXPECT_EQ("b", parts[2]);
}

// ---------------- AnimationCommand ----------------

TEST(AnimationCommands, AnimationCommandParsesMaestroTemplate)
{
    // MODULE_TYPE::MAESTRO = 1, duration 500ms, module index 0
    AnimationCommand cmd("1|500|0|ctrl|3|75|100|50");
    EXPECT_EQ(MODULE_TYPE::MAESTRO, cmd.commandType);
    EXPECT_EQ(500, cmd.duration);
    EXPECT_EQ(0, cmd.module);
}

TEST(AnimationCommands, AnimationCommandParsesGpioTemplate)
{
    // MODULE_TYPE::GPIO = 5, duration 100ms, module index 2
    AnimationCommand cmd("5|100|2|1|1");
    EXPECT_EQ(MODULE_TYPE::GPIO, cmd.commandType);
    EXPECT_EQ(100, cmd.duration);
    EXPECT_EQ(2, cmd.module);
}

TEST(AnimationCommands, AnimationCommandParsesSerialTemplate)
{
    // MODULE_TYPE::GENERIC_SERIAL = 3
    AnimationCommand cmd("3|200|0|1|9600|hello");
    EXPECT_EQ(MODULE_TYPE::GENERIC_SERIAL, cmd.commandType);
    EXPECT_EQ(200, cmd.duration);
    EXPECT_EQ(0, cmd.module);
}

// ---------------- CommandTemplate ----------------

TEST(AnimationCommands, GetCommandTemplatePtrReturnsCorrectFields)
{
    AnimationCommand cmd("1|500|0|ctrl|3|75|100|50");
    auto tmpl = cmd.GetCommandTemplatePtr();

    ASSERT_NE(nullptr, tmpl);
    EXPECT_EQ(MODULE_TYPE::MAESTRO, tmpl->type);
    EXPECT_EQ(0, tmpl->module);
    EXPECT_EQ("1|500|0|ctrl|3|75|100|50", tmpl->val);
}

// ---------------- MaestroCommand ----------------

TEST(AnimationCommands, MaestroCommandParsesAllFields)
{
    // type|X|controller|channel|position|speed|acceleration
    MaestroCommand cmd("1|500|myctrl|3|75|100|50");
    EXPECT_EQ("myctrl", cmd.controller);
    EXPECT_EQ(3, cmd.channel);
    EXPECT_EQ(75, cmd.position);
    EXPECT_EQ(100, cmd.speed);
    EXPECT_EQ(50, cmd.acceleration);
}

TEST(AnimationCommands, MaestroCommandTooFewPartsSetsSafeDefaults)
{
    MaestroCommand cmd("1|500|myctrl");
    EXPECT_EQ(-1, cmd.channel);
    EXPECT_EQ(-1, cmd.position);
    EXPECT_EQ(-1, cmd.speed);
    EXPECT_EQ(-1, cmd.acceleration);
}

// ---------------- I2cCommand ----------------

TEST(AnimationCommands, I2cCommandParsesChannelAndValue)
{
    // type|X|channel|value
    I2cCommand cmd("2|300|5|some_data");
    EXPECT_EQ(5, cmd.channel);
    EXPECT_EQ("some_data", cmd.value);
}

TEST(AnimationCommands, I2cCommandTooFewPartsSetsSafeDefaults)
{
    I2cCommand cmd("2|300");
    EXPECT_EQ(-1, cmd.channel);
    EXPECT_EQ("", cmd.value);
}

// ---------------- GpioCommand ----------------

TEST(AnimationCommands, GpioCommandParsesChannelAndState)
{
    // type|X|channel|state(0 or 1)
    GpioCommand cmd("5|100|2|1");
    EXPECT_EQ(2, cmd.channel);
    EXPECT_TRUE(cmd.state);
}

TEST(AnimationCommands, GpioCommandStateFalse)
{
    GpioCommand cmd("5|100|3|0");
    EXPECT_EQ(3, cmd.channel);
    EXPECT_FALSE(cmd.state);
}

TEST(AnimationCommands, GpioCommandTooFewPartsSetsSafeDefaults)
{
    GpioCommand cmd("5");
    EXPECT_EQ(-1, cmd.channel);
    EXPECT_FALSE(cmd.state);
}

// ---------------- SerialCommand ----------------

TEST(AnimationCommands, SerialCommandParsesGenericSerial)
{
    // GENERIC_SERIAL=3, type|X|serialChannel|baudRate|value
    SerialCommand cmd("3|200|1|9600|hello");
    EXPECT_EQ(MODULE_TYPE::GENERIC_SERIAL, cmd.type);
    EXPECT_EQ(1, cmd.serialChannel);
    EXPECT_EQ(9600, cmd.baudRate);
    EXPECT_EQ("hello", cmd.GetValue());
}

TEST(AnimationCommands, SerialCommandParsesKangarooFields)
{
    // KANGAROO=4, type|X|serialChannel|baudRate|ch|cmd|spd|pos
    SerialCommand cmd("4|300|1|9600|2|3|50|100");
    EXPECT_EQ(MODULE_TYPE::KANGAROO, cmd.type);
    EXPECT_EQ(1, cmd.serialChannel);
    EXPECT_EQ(9600, cmd.baudRate);
}

TEST(AnimationCommands, SerialCommandKangarooStartAction)
{
    // KangarooAction::START = 0
    SerialCommand cmd("4|300|1|9600|2|0|0|0");
    std::string val = cmd.GetValue();
    EXPECT_EQ("2,start\n", val);
}

TEST(AnimationCommands, SerialCommandKangarooHomeAction)
{
    // KangarooAction::HOME = 1
    SerialCommand cmd("4|300|1|9600|3|1|0|0");
    std::string val = cmd.GetValue();
    EXPECT_EQ("3,home\n", val);
}

TEST(AnimationCommands, SerialCommandKangarooSpeedAction)
{
    // KangarooAction::SPEED = 2
    SerialCommand cmd("4|300|1|9600|1|2|50|0");
    std::string val = cmd.GetValue();
    EXPECT_EQ("1,s50\n", val);
}

TEST(AnimationCommands, SerialCommandKangarooPositionWithSpeed)
{
    // KangarooAction::POSITION = 3, spd > 0
    SerialCommand cmd("4|300|1|9600|1|3|50|200");
    std::string val = cmd.GetValue();
    EXPECT_EQ("1,p200 s50\n", val);
}

TEST(AnimationCommands, SerialCommandKangarooPositionNoSpeed)
{
    // KangarooAction::POSITION = 3, spd = 0
    SerialCommand cmd("4|300|1|9600|1|3|0|200");
    std::string val = cmd.GetValue();
    EXPECT_EQ("1,p200\n", val);
}

TEST(AnimationCommands, SerialCommandTooFewPartsSetsSafeDefaults)
{
    SerialCommand cmd("3|200");
    EXPECT_EQ(MODULE_TYPE::NONE, cmd.type);
    EXPECT_EQ(-1, cmd.serialChannel);
    EXPECT_EQ(-1, cmd.baudRate);
    EXPECT_EQ("", cmd.value);
}

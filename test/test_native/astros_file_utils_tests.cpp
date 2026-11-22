#include <AstrOsFileUtils.hpp>
#include <AstrOsLogger.hpp>
#include <AstrOsStringUtils.hpp>
#include <cstdarg>
#include <cstdio>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

TEST(FileUtils, ParseMaestroConfig)
{
    std::string maestroFile = "0:1:115200\n1:2:9600\n# Comment line\n2:3:4800\n";
    auto configs = AstrOsFileUtils::parseMaestroConfig(maestroFile);

    EXPECT_EQ(3, configs.size());

    EXPECT_EQ(0, configs[0].idx);
    EXPECT_EQ(1, configs[0].uartChannel);
    EXPECT_EQ(115200, configs[0].baudrate);

    EXPECT_EQ(1, configs[1].idx);
    EXPECT_EQ(2, configs[1].uartChannel);
    EXPECT_EQ(9600, configs[1].baudrate);

    EXPECT_EQ(2, configs[2].idx);
    EXPECT_EQ(3, configs[2].uartChannel);
    EXPECT_EQ(4800, configs[2].baudrate);
}

TEST(FileUtils, ParseServoConfig)
{
    std::string servoFile = "0:1:0:500:2500:1500:0|1:1:1:1000:2000:1250:1|2:1:1:750:2250:1000:0|";
    auto channels = AstrOsFileUtils::parseServoConfig(servoFile);

    EXPECT_EQ(3, channels.size());

    EXPECT_EQ(0, channels[0].id);
    EXPECT_TRUE(channels[0].enabled);
    EXPECT_FALSE(channels[0].isServo);
    EXPECT_EQ(500, channels[0].minPos);
    EXPECT_EQ(2500, channels[0].maxPos);
    EXPECT_EQ(1500, channels[0].home);
    EXPECT_EQ(500, channels[0].currentPos);
    EXPECT_EQ(1500, channels[0].requestedPos);
    EXPECT_EQ(500, channels[0].lastPos);
    EXPECT_EQ(0, channels[0].speed);
    EXPECT_EQ(0, channels[0].acceleration);
    EXPECT_FALSE(channels[0].inverted);
    EXPECT_FALSE(channels[0].on);

    EXPECT_EQ(1, channels[1].id);
    EXPECT_TRUE(channels[1].enabled);
    EXPECT_TRUE(channels[1].isServo);
    EXPECT_EQ(1000, channels[1].minPos);
    EXPECT_EQ(2000, channels[1].maxPos);
    EXPECT_EQ(1250, channels[1].home);
    EXPECT_EQ(1000, channels[1].currentPos);
    EXPECT_EQ(1250, channels[1].requestedPos);
    EXPECT_EQ(1000, channels[1].lastPos);
    EXPECT_EQ(0, channels[1].speed);
    EXPECT_EQ(0, channels[1].acceleration);
    EXPECT_TRUE(channels[1].inverted);
    EXPECT_FALSE(channels[1].on);

    EXPECT_EQ(2, channels[2].id);
    EXPECT_TRUE(channels[2].enabled);
    EXPECT_TRUE(channels[2].isServo);
    EXPECT_EQ(750, channels[2].minPos);
    EXPECT_EQ(2250, channels[2].maxPos);
    EXPECT_EQ(1000, channels[2].home);
    EXPECT_EQ(750, channels[2].currentPos);
    EXPECT_EQ(1000, channels[2].requestedPos);
    EXPECT_EQ(750, channels[2].lastPos);
    EXPECT_EQ(0, channels[2].speed);
    EXPECT_EQ(0, channels[2].acceleration);
    EXPECT_FALSE(channels[2].inverted);
    EXPECT_FALSE(channels[2].on);
}

// T-010: the 8th channel field is releaseMs (.docs/protocol.md § D).
namespace
{
    int g_releaseWarnings = 0;
    std::string g_lastReleaseWarning;

    void captureReleaseWarning(const char *, const char *fmt, va_list args)
    {
        char buf[256];
        vsnprintf(buf, sizeof(buf), fmt, args);
        g_releaseWarnings++;
        g_lastReleaseWarning = buf;
    }

    AstrOsLogger releaseWarningCapture()
    {
        g_releaseWarnings = 0;
        g_lastReleaseWarning.clear();
        AstrOsLogger logger;
        logger.warn = captureReleaseWarning;
        return logger;
    }
} // namespace

TEST(FileUtils, ParseServoConfigDefaultsReleaseTimeForSevenFields)
{
    // Break: a 1.2.x SD file (7 fields) losing its 5 s floor on a 1.3.0 board.
    auto channels = AstrOsFileUtils::parseServoConfig("0:1:1:500:2500:1500:0|1:1:0:1000:2000:1250:1|");
    ASSERT_EQ(2u, channels.size());
    EXPECT_EQ(5000, channels[0].releaseMs);
    EXPECT_EQ(5000, channels[1].releaseMs);
}

TEST(FileUtils, ParseServoConfigReadsTheEighthFieldAsReleaseTime)
{
    auto channels = AstrOsFileUtils::parseServoConfig(
        "0:1:1:500:2500:1500:0:15000|1:1:1:1000:2000:1250:0:1000|2:1:1:1000:2000:1250:1:120000");
    ASSERT_EQ(3u, channels.size());
    EXPECT_EQ(15000, channels[0].releaseMs);
    EXPECT_EQ(1000, channels[1].releaseMs);
    EXPECT_EQ(120000, channels[2].releaseMs);
    EXPECT_EQ(500, channels[0].minPos); // the first 7 fields still parse
    EXPECT_TRUE(channels[2].inverted);
}

TEST(FileUtils, ParseServoConfigKeepsAChannelWithABadReleaseTime)
{
    // Break: a bad optional field deleting the channel (a typo would make it
    // silently stop working) instead of falling back to the default.
    auto logger = releaseWarningCapture();
    auto channels = AstrOsFileUtils::parseServoConfig(
        "0:1:1:500:2500:1500:0:abc|1:1:1:500:2500:1500:0:999|2:1:1:500:2500:1500:0:120001|"
        "3:1:1:500:2500:1500:0:15000x",
        logger);
    ASSERT_EQ(4u, channels.size());
    for (int i = 0; i < 4; i++)
    {
        EXPECT_EQ(i, channels[i].id);
        EXPECT_TRUE(channels[i].enabled);
        EXPECT_EQ(5000, channels[i].releaseMs);
    }
    EXPECT_EQ(4, g_releaseWarnings);
    EXPECT_NE(std::string::npos, g_lastReleaseWarning.find("channel 3")) << g_lastReleaseWarning;
}

TEST(FileUtils, ParseServoConfigWarnsOnAnEmptyEighthField)
{
    // Break: "…:0:" (what a server mapping bug that drops releaseMs emits, since
    // JS joins undefined as '') defaulting silently, so the setting never applies.
    auto logger = releaseWarningCapture();
    auto channels = AstrOsFileUtils::parseServoConfig("0:1:1:500:2500:1500:0:|1:1:1:500:2500:1500:0:\n", logger);
    ASSERT_EQ(2u, channels.size());
    EXPECT_TRUE(channels[0].enabled);
    EXPECT_EQ(5000, channels[0].releaseMs);
    EXPECT_EQ(5000, channels[1].releaseMs);
    EXPECT_EQ(2, g_releaseWarnings);
}

TEST(FileUtils, ParseServoConfigToleratesTrailingWhitespace)
{
    // Break: a hand-edited SD file saved with a final newline (vim, nano) losing
    // its last channel's release time; 1.2.x tolerated trailing whitespace.
    auto logger = releaseWarningCapture();
    auto channels =
        AstrOsFileUtils::parseServoConfig("0:1:1:500:2500:1500:0:15000|1:1:1:500:2500:1500:0:7000\r\n", logger);
    ASSERT_EQ(2u, channels.size());
    EXPECT_EQ(15000, channels[0].releaseMs);
    EXPECT_EQ(7000, channels[1].releaseMs);
    EXPECT_EQ(0, g_releaseWarnings);
}

TEST(FileUtils, ParseServoConfigNamesTheBadValueInTheWarning)
{
    auto logger = releaseWarningCapture();
    AstrOsFileUtils::parseServoConfig("4:1:1:500:2500:1500:0:999", logger);
    EXPECT_EQ(1, g_releaseWarnings);
    EXPECT_NE(std::string::npos, g_lastReleaseWarning.find("channel 4")) << g_lastReleaseWarning;
    EXPECT_NE(std::string::npos, g_lastReleaseWarning.find("999")) << g_lastReleaseWarning;
}

TEST(FileUtils, ParseServoConfigIgnoresFieldsAfterTheEighth)
{
    // Break: a future 9th field wiping channels on 1.3.0, as an 8th does on 1.2.x.
    auto channels = AstrOsFileUtils::parseServoConfig("0:1:1:500:2500:1500:0:15000:99|1:1:1:500:2500:1500:0:7000:x:y");
    ASSERT_EQ(2u, channels.size());
    EXPECT_EQ(15000, channels[0].releaseMs);
    EXPECT_EQ(7000, channels[1].releaseMs);
    EXPECT_TRUE(channels[1].enabled);
}

TEST(FileUtils, ParseServoConfigStillDropsShortOrBadRequiredEntries)
{
    // 6 fields (id 0) and a bad minPos (id 2) are dropped as before; id 1 parses.
    auto channels =
        AstrOsFileUtils::parseServoConfig("0:1:1:500:2500:1500|1:1:1:500:2500:1500:0:15000|2:1:1:x:2500:1500:0:5000");
    ASSERT_EQ(3u, channels.size()); // max id 2: a bad required field still counts toward the size
    EXPECT_FALSE(channels[0].enabled);
    EXPECT_EQ(5000, channels[0].releaseMs); // dropped slots keep the default, never 0 ms
    EXPECT_EQ(15000, channels[1].releaseMs);
    EXPECT_FALSE(channels[2].enabled);
    EXPECT_EQ(5000, channels[2].releaseMs);
}

TEST(FileUtils, ParseServoConfigCountsAChannelWithABadReleaseTimeTowardTheSize)
{
    // Break: the max-id pass and the parse pass disagreeing on validity.
    auto channels = AstrOsFileUtils::parseServoConfig("5:1:1:500:2500:1500:0:abc|7:1:1:500");
    ASSERT_EQ(6u, channels.size());
    EXPECT_TRUE(channels[5].enabled);
    EXPECT_EQ(5000, channels[5].releaseMs);
}

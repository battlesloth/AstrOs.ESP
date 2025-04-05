#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <AstrOsStringUtils.hpp>
#include <AstrOsFileUtils.hpp>

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

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <AstrOsServoUtils.hpp>

TEST(ServoUtils, GetMicroSecondsAsStep)
{
    auto freq = 50.0; // 50 Hz
    auto freq_us = 1000000.0 / freq; // frequency in microseconds

    auto step1 = GetMicroSecondsAsStep(500, freq_us);
    auto step2 = GetMicroSecondsAsStep(1200, freq_us);
    auto step3 = GetMicroSecondsAsStep(2500, freq_us);

    EXPECT_EQ(102, step1);
    EXPECT_EQ(246, step2);
    EXPECT_EQ(512, step3);
}

TEST(ServoUtils, GetMicroSecondsAsMapPosition)
{
    auto freq = 50; // 50 Hz
    auto freq_us = 1000000l / freq; // frequency in microseconds

    auto positionA = MicroSecondsToMapPosition(500, freq_us, 102, 512, 360);
    auto positionB = MicroSecondsToMapPosition(1200, freq_us, 102, 512, 360);
    auto positionC = MicroSecondsToMapPosition(2500, freq_us, 102, 512, 360);
    
    EXPECT_EQ(0, positionA);
    EXPECT_EQ(126, positionB);
    EXPECT_EQ(359, positionC);
}

TEST(ServoUtils, CalculateStepMap)
{
    auto map = new uint16_t[360];

    auto step = CalculateStepMap(50, map, 360);

    EXPECT_EQ(1.1389, std::round(step * 10000 ) / 10000.0);

    EXPECT_EQ(102, (int)map[0]);

    EXPECT_EQ(203, (int)map[89]);

    EXPECT_EQ(511, (int)map[359]);

    delete[] map;
}

TEST(ServoUtils, GetRelativeRequestedPosition)
{    
    auto positionA = GetRelativeRequestedPosition(0, 359, 0);
    auto positionB = GetRelativeRequestedPosition(0, 359, 50);
    auto positionC = GetRelativeRequestedPosition(0, 359, 100);


    auto positionD = GetRelativeRequestedPosition(33, 326, 0);
    auto positionE = GetRelativeRequestedPosition(33, 326, 50);
    auto positionF = GetRelativeRequestedPosition(33, 326, 100);

    auto positionG = GetRelativeRequestedPosition(179, 326, 0);
    auto positionH = GetRelativeRequestedPosition(179, 326, 50);
    auto positionI = GetRelativeRequestedPosition(179, 326, 100);
    
    EXPECT_EQ(0, positionA);
    EXPECT_EQ(179, positionB);
    EXPECT_EQ(359, positionC);

    EXPECT_EQ(33, positionD);
    EXPECT_EQ(179, positionE);
    EXPECT_EQ(326, positionF);

    EXPECT_EQ(179, positionG);
    EXPECT_EQ(252, positionH);
    EXPECT_EQ(326, positionI);
}

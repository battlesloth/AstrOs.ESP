#include <AstrOsServoUtils.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

TEST(ServoUtils, GetMicroSecondsAsStep)
{
    auto freq = 50.0;                // 50 Hz
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
    auto freq = 50;                 // 50 Hz
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

    EXPECT_EQ(1.1389, std::round(step * 10000) / 10000.0);

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

// T-001: worst-case travel deadline for the Maestro release check.
// Model: 0.25 us per 10 ms per speed unit over a 0-3000 us guard range
// => 120000 / effectiveSpeed ms, times MAESTRO_RELEASE_SLACK (4), ceiling.
TEST(ServoUtils, WorstCaseTravelMsUnlimitedSpeedTreatedAs255)
{
    EXPECT_EQ(1883, WorstCaseTravelMs(0, 0));
    EXPECT_EQ(1883, WorstCaseTravelMs(255, 0));
}

TEST(ServoUtils, WorstCaseTravelMsSlowestSpeedIsFinite)
{
    // speed 1 formerly never released (fractional increment truncated to 0)
    EXPECT_EQ(480000, WorstCaseTravelMs(1, 0));
}

TEST(ServoUtils, WorstCaseTravelMsMidSpeed)
{
    EXPECT_EQ(48000, WorstCaseTravelMs(10, 0));
}

TEST(ServoUtils, WorstCaseTravelMsAccelSubstitutesWhenSlower)
{
    // accel 1 with a fast speed formerly never released
    EXPECT_EQ(480000, WorstCaseTravelMs(200, 1));
    // accel substitutes even when speed is unlimited (0 -> 255)
    EXPECT_EQ(160000, WorstCaseTravelMs(0, 3));
}

TEST(ServoUtils, WorstCaseTravelMsAccelIgnoredWhenNotSlower)
{
    EXPECT_EQ(48000, WorstCaseTravelMs(10, 50));
    EXPECT_EQ(48000, WorstCaseTravelMs(10, 10));
    EXPECT_EQ(48000, WorstCaseTravelMs(10, 0));
}

TEST(ServoUtils, WorstCaseTravelMsClampsOutOfRangeInputs)
{
    // negative or >255 values from a bad parse behave like the nearest bound
    EXPECT_EQ(1883, WorstCaseTravelMs(-5, -1));
    EXPECT_EQ(1883, WorstCaseTravelMs(999, 300));
}

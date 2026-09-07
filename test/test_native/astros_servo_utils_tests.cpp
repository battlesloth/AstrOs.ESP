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
// Physical model over the 0-3000 us guard range, true Maestro units:
//   speed unit = 0.25 us / 10 ms  -> cruise = 120000 / speed ms
//   accel unit = 0.25 us / 10 ms / 80 ms -> speed grows `accel` units per 80 ms
// Trapezoid (cap reached, speed^2 <= 1500*accel): cruise + 80*speed/accel.
// Triangle (cap never reached):                   sqrt(38400000 / accel).
// No multiplier: the guard range (3000 us vs a real sweep of <= 2000 us) is
// the margin. All ceilings.
TEST(ServoUtils, WorstCaseTravelMsSpeedOnlyUnlimitedTreatedAs255)
{
    EXPECT_EQ(471, WorstCaseTravelMs(0, 0));
    EXPECT_EQ(471, WorstCaseTravelMs(255, 0));
}

TEST(ServoUtils, WorstCaseTravelMsSpeedOnlySlowestIsFinite)
{
    // speed 1 formerly never released (fractional increment truncated to 0)
    EXPECT_EQ(120000, WorstCaseTravelMs(1, 0));
}

TEST(ServoUtils, WorstCaseTravelMsSpeedOnlyMidRange)
{
    EXPECT_EQ(12000, WorstCaseTravelMs(10, 0));
    EXPECT_EQ(6000, WorstCaseTravelMs(20, 0));
}

TEST(ServoUtils, WorstCaseTravelMsTrapezoidAddsRampToCruise)
{
    // accel only adds a ramp; it is not a speed cap
    EXPECT_EQ(6800, WorstCaseTravelMs(20, 2));  // 6000 + 800
    EXPECT_EQ(7600, WorstCaseTravelMs(20, 1));  // 6000 + 1600
    EXPECT_EQ(12400, WorstCaseTravelMs(10, 2)); // 12000 + 400
    EXPECT_EQ(24400, WorstCaseTravelMs(5, 1));  // 24000 + 400
}

TEST(ServoUtils, WorstCaseTravelMsTrapezoidRampCeilsAndIsSmallForHighAccel)
{
    EXPECT_EQ(12016, WorstCaseTravelMs(10, 50)); // 12000 + ceil(800/50)=16
    EXPECT_EQ(120080, WorstCaseTravelMs(1, 1));  // 120000 + 80
}

TEST(ServoUtils, WorstCaseTravelMsTriangleWhenCapNeverReached)
{
    // unlimited speed with low accel: servo never reaches speed 255
    EXPECT_EQ(6197, WorstCaseTravelMs(0, 1)); // ceil(sqrt(38400000))
    EXPECT_EQ(4382, WorstCaseTravelMs(0, 2)); // ceil(sqrt(19200000))
    EXPECT_EQ(2772, WorstCaseTravelMs(0, 5)); // ceil(sqrt(7680000))
    // accel 1 with a fast speed formerly never released
    EXPECT_EQ(6197, WorstCaseTravelMs(200, 1));
}

TEST(ServoUtils, WorstCaseTravelMsRegimeBoundaryIsContinuous)
{
    // 255^2 = 65025: accel 43 (1500*43 = 64500) is triangle, accel 44 (66000) is trapezoid
    EXPECT_EQ(945, WorstCaseTravelMs(255, 43));
    EXPECT_EQ(935, WorstCaseTravelMs(255, 44));
}

TEST(ServoUtils, WorstCaseTravelMsClampsOutOfRangeInputs)
{
    // negative or >255 values from a bad parse behave like the nearest bound
    EXPECT_EQ(471, WorstCaseTravelMs(-5, -1));
    EXPECT_EQ(551, WorstCaseTravelMs(999, 300)); // (255,255): 471 + 80
}

// Policy layer: the deadline CheckServos actually uses (T-005): the greater
// of a 5 s floor and the physical model plus 10 % (ceiling).
TEST(ServoUtils, ServoReleaseDeadlineMsAppliesFloorWhenModelIsFaster)
{
    EXPECT_EQ(5000, ServoReleaseDeadlineMs(0, 0));    // model 471  -> 519
    EXPECT_EQ(5000, ServoReleaseDeadlineMs(255, 44)); // model 935  -> 1029
    EXPECT_EQ(5000, ServoReleaseDeadlineMs(0, 5));    // model 2772 -> 3050
}

TEST(ServoUtils, ServoReleaseDeadlineMsAddsTenPercentWhenModelIsSlower)
{
    EXPECT_EQ(6600, ServoReleaseDeadlineMs(20, 0));  // 6000 + 600
    EXPECT_EQ(7480, ServoReleaseDeadlineMs(20, 2));  // 6800 + 680 (the reported bug case)
    EXPECT_EQ(13640, ServoReleaseDeadlineMs(10, 2)); // 12400 + 1240
    EXPECT_EQ(26400, ServoReleaseDeadlineMs(5, 0));  // 24000 + 2400
    EXPECT_EQ(26840, ServoReleaseDeadlineMs(5, 1));  // 24400 + 2440
    EXPECT_EQ(132000, ServoReleaseDeadlineMs(1, 0)); // 120000 + 12000
}

TEST(ServoUtils, ServoReleaseDeadlineMsMarginCeilsAndCrossesFloorNearFourAndAHalfSeconds)
{
    // 10 % of 4382 is 438.2 -> 439; 4821 < 5000 so the floor wins
    EXPECT_EQ(5000, ServoReleaseDeadlineMs(0, 2)); // model 4382
    // speed 26 -> cruise ceil(120000/26) = 4616; +462 = 5078 > 5000 so the model wins
    EXPECT_EQ(5078, ServoReleaseDeadlineMs(26, 0));
}

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

// Policy layer: the deadline CheckServos actually uses: the greater of the
// channel's release floor (T-010; MAESTRO_RELEASE_DEFAULT_MS = 5 s, T-005's
// floor) and the physical model plus 10 % (ceiling).
TEST(ServoUtils, ServoReleaseDeadlineMsAppliesFloorWhenModelIsFaster)
{
    EXPECT_EQ(5000, ServoReleaseDeadlineMs(0, 0, MAESTRO_RELEASE_DEFAULT_MS));    // model 471  -> 519
    EXPECT_EQ(5000, ServoReleaseDeadlineMs(255, 44, MAESTRO_RELEASE_DEFAULT_MS)); // model 935  -> 1029
    EXPECT_EQ(5000, ServoReleaseDeadlineMs(0, 5, MAESTRO_RELEASE_DEFAULT_MS));    // model 2772 -> 3050
}

TEST(ServoUtils, ServoReleaseDeadlineMsAddsTenPercentWhenModelIsSlower)
{
    EXPECT_EQ(6600, ServoReleaseDeadlineMs(20, 0, MAESTRO_RELEASE_DEFAULT_MS));  // 6000 + 600
    EXPECT_EQ(7480, ServoReleaseDeadlineMs(20, 2, MAESTRO_RELEASE_DEFAULT_MS));  // 6800 + 680 (the reported bug case)
    EXPECT_EQ(13640, ServoReleaseDeadlineMs(10, 2, MAESTRO_RELEASE_DEFAULT_MS)); // 12400 + 1240
    EXPECT_EQ(26400, ServoReleaseDeadlineMs(5, 0, MAESTRO_RELEASE_DEFAULT_MS));  // 24000 + 2400
    EXPECT_EQ(26840, ServoReleaseDeadlineMs(5, 1, MAESTRO_RELEASE_DEFAULT_MS));  // 24400 + 2440
    EXPECT_EQ(132000, ServoReleaseDeadlineMs(1, 0, MAESTRO_RELEASE_DEFAULT_MS)); // 120000 + 12000
}

TEST(ServoUtils, ServoReleaseDeadlineMsMarginCeilsAndCrossesFloorNearFourAndAHalfSeconds)
{
    // 10 % of 4382 is 438.2 -> 439; 4821 < 5000 so the floor wins
    EXPECT_EQ(5000, ServoReleaseDeadlineMs(0, 2, MAESTRO_RELEASE_DEFAULT_MS)); // model 4382
    // speed 26 -> cruise ceil(120000/26) = 4616; +462 = 5078 > 5000 so the model wins
    EXPECT_EQ(5078, ServoReleaseDeadlineMs(26, 0, MAESTRO_RELEASE_DEFAULT_MS));
}

// T-010: the floor is the channel's own release time. It is a floor, never a
// cap: a slow move still gets its modeled travel plus 10 %.
TEST(ServoUtils, ServoReleaseDeadlineMsUsesTheChannelFloorWhenItExceedsTheModel)
{
    EXPECT_EQ(15000, ServoReleaseDeadlineMs(0, 0, 15000)); // model 519: an actuator's 15 s wins
    EXPECT_EQ(30000, ServoReleaseDeadlineMs(5, 0, 30000)); // model 26400 < 30000
    EXPECT_EQ(1000, ServoReleaseDeadlineMs(0, 0, 1000));   // a floor below 5 s releases sooner
}

TEST(ServoUtils, ServoReleaseDeadlineMsKeepsTheModelWhenItExceedsTheChannelFloor)
{
    EXPECT_EQ(26400, ServoReleaseDeadlineMs(5, 0, 10000)); // 24000 + 2400 > 10000
    EXPECT_EQ(6600, ServoReleaseDeadlineMs(20, 0, 6000));  // 6000 + 600 > 6000
    EXPECT_EQ(5078, ServoReleaseDeadlineMs(26, 0, 1000));  // 4616 + 462 > 1000
}

// Maestro Set Target payload (T-006): the target is in quarter-microseconds,
// split low 7 bits / high 7 bits. Both the pre-position and the target frame
// in MaestroModule::setServoPosition go through this, so a µs value encodes
// identically wherever it is used; 0 (servo off) stays 0.
TEST(ServoUtils, EncodeMaestroTargetZeroIsOff)
{
    uint8_t lo = 0xFF, hi = 0xFF;
    EncodeMaestroTarget(0, lo, hi);
    EXPECT_EQ(0x00, lo);
    EXPECT_EQ(0x00, hi);
}

TEST(ServoUtils, EncodeMaestroTargetScalesToQuarterMicroseconds)
{
    uint8_t lo = 0, hi = 0;

    EncodeMaestroTarget(500, lo, hi); // 2000
    EXPECT_EQ(0x50, lo);
    EXPECT_EQ(0x0F, hi);

    EncodeMaestroTarget(1500, lo, hi); // 6000 -- a raw 1500 would be 0x5C 0x0B (375 us)
    EXPECT_EQ(0x70, lo);
    EXPECT_EQ(0x2E, hi);

    EncodeMaestroTarget(2500, lo, hi); // 10000
    EXPECT_EQ(0x10, lo);
    EXPECT_EQ(0x4E, hi);
}

// T-010: MaestroModule holds `servo_channel channels[24] = {};`. A
// `{}`-initialized channel must release on the 5 s default, never on a 0 ms floor.
TEST(ServoUtils, ABraceInitializedChannelArrayReadsTheDefaultReleaseTime)
{
    servo_channel channels[3] = {};
    for (const auto &ch : channels)
    {
        EXPECT_EQ(MAESTRO_RELEASE_DEFAULT_MS, ch.releaseMs);
        EXPECT_EQ(5000, ch.releaseMs);
    }
}

// T-010 amendment (Jeff): an out-of-range floor (a memset struct, a future
// writer) falls back to the default instead of releasing at the model alone.
TEST(ServoUtils, ServoReleaseDeadlineMsClampsAnOutOfRangeFloorToTheDefault)
{
    EXPECT_EQ(5000, ServoReleaseDeadlineMs(0, 0, 0));      // model 519: would release at ~0.5 s
    EXPECT_EQ(5000, ServoReleaseDeadlineMs(0, 0, 999));    // just below MIN
    EXPECT_EQ(5000, ServoReleaseDeadlineMs(0, 0, 120001)); // just above MAX
    EXPECT_EQ(5000, ServoReleaseDeadlineMs(0, 0, -1));
    EXPECT_EQ(120000, ServoReleaseDeadlineMs(0, 0, 120000)); // MAX itself is in range
}

namespace
{
    servo_channel channel(bool enabled, bool isServo, bool on)
    {
        servo_channel ch = {};
        ch.enabled = enabled;
        ch.isServo = isServo;
        ch.on = on;
        return ch;
    }
} // namespace

// T-010 amendment (Jeff): after a reload, a servo left energized but no longer
// tracked (disabled by the new config, or not re-armed because homing failed)
// must be sent an off, or it stays powered until power-cycle.
TEST(ServoUtils, ChannelsToReleaseAfterReloadPicksEnergizedChannelsHomingDidNotRearm)
{
    servo_channel channels[6] = {
        channel(false, true, false), // 0: was energized, now disabled -> release
        channel(true, true, true),   // 1: was energized, homing re-armed it -> keep
        channel(true, true, false),  // 2: was energized, homing failed to re-arm -> release
        channel(true, false, false), // 3: was energized, now an enabled digital output -> keep (homing set its level)
        channel(false, true, false), // 4: not energized before -> nothing to do
        channel(false, false, false) // 5: was energized, now disabled digital -> release
    };
    uint32_t wasEnergized = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 5);
    EXPECT_EQ((1u << 0) | (1u << 2) | (1u << 5), ChannelsToReleaseAfterReload(wasEnergized, channels, 6));
    EXPECT_EQ(0u, ChannelsToReleaseAfterReload(0, channels, 6));
}

// T-010: one INFO line per module shows the release times a board applied, so
// a log-only padawan can prove what it parsed (QA case 21).
TEST(ServoUtils, FormatReleaseTimesListsEnabledServosOffTheDefault)
{
    servo_channel channels[4] = {};
    channels[0] = channel(true, true, false);
    channels[1] = channel(true, true, false);
    channels[1].releaseMs = 15000;
    channels[2] = channel(true, false, false); // digital: never released, not listed
    channels[2].releaseMs = 30000;
    channels[3] = channel(false, true, false); // disabled: not listed
    channels[3].releaseMs = 30000;
    EXPECT_EQ("ch1 15000 ms; others 5000 ms", FormatReleaseTimes(channels, 4));

    servo_channel defaults[2] = {channel(true, true, false), channel(true, true, false)};
    EXPECT_EQ("all 5000 ms (default)", FormatReleaseTimes(defaults, 2));
}

#include <AstrOsEnums.h>
#include <GpioCommand.hpp>
#include <MaestroCommand.hpp>
#include <RandomCommands.hpp>
#include <RandomEffects.hpp>
#include <RandomSpanAdmission.hpp>
#include <gtest/gtest.h>

#include <climits>
#include <cstdint>
#include <cstring>
#include <string>

// The random-effects task's pure checks and formatting (T-009 task file, Rulings).
// Behavior: .docs/protocol.md § C.

namespace
{
    // A servo channel spanning 1000-2000 us, last commanded to its middle.
    MaestroChannelSnapshot ServoChannel()
    {
        MaestroChannelSnapshot ch{};
        ch.enabled = true;
        ch.isServo = true;
        ch.minPos = 1000;
        ch.maxPos = 2000;
        ch.home = 1500;
        ch.inverted = false;
        ch.lastPos = 1500;
        return ch;
    }

    MaestroChannelSnapshot DigitalChannel(int lastPos)
    {
        MaestroChannelSnapshot ch{};
        ch.enabled = true;
        ch.isServo = false;
        ch.home = 500;
        ch.lastPos = lastPos;
        return ch;
    }

    // Every field distinct, so a swapped copy shows.
    RandomMaestroCommand ServoCommand()
    {
        RandomMaestroCommand cmd{};
        cmd.module = 1;
        cmd.channel = 4;
        cmd.spanMs = 5000;
        cmd.minGapMs = 200;
        cmd.maxGapMs = 800;
        cmd.minPos = 35;
        cmd.maxPos = 65;
        cmd.minMove = 2;
        cmd.maxMove = 8;
        cmd.speed = 10;
        cmd.acceleration = 3;
        return cmd;
    }

    // What the server sends for a digital channel: servo fields all 0.
    RandomMaestroCommand DigitalCommand(int spanMs = 10000, int minGapMs = 50, int maxGapMs = 400)
    {
        RandomMaestroCommand cmd{};
        cmd.module = 2;
        cmd.channel = 7;
        cmd.spanMs = spanMs;
        cmd.minGapMs = minGapMs;
        cmd.maxGapMs = maxGapMs;
        return cmd;
    }

    RandomGpioCommand GpioCommandFor(int channel, int spanMs = 10000, int minGapMs = 50, int maxGapMs = 400)
    {
        RandomGpioCommand cmd{};
        cmd.channel = channel;
        cmd.spanMs = spanMs;
        cmd.minGapMs = minGapMs;
        cmd.maxGapMs = maxGapMs;
        return cmd;
    }

    void ExpectKey(const SpanKey &key, int type, int module, int channel)
    {
        EXPECT_EQ(key.type, type);
        EXPECT_EQ(key.module, module);
        EXPECT_EQ(key.channel, channel);
    }

    void ExpectNoServoFields(const SpanStart &s)
    {
        EXPECT_EQ(s.minPos, 0);
        EXPECT_EQ(s.maxPos, 0);
        EXPECT_EQ(s.minMove, 0);
        EXPECT_EQ(s.maxMove, 0);
        EXPECT_EQ(s.speed, 0);
        EXPECT_EQ(s.acceleration, 0);
        EXPECT_EQ(s.rangeUs, 0);
        EXPECT_EQ(s.initialPct, 0);
    }

    std::string Format(const SpanChange &change)
    {
        char buf[SPAN_CHANGE_MAX_LEN];
        size_t len = FormatSpanChange(change, buf, sizeof(buf));
        return std::string(buf, len);
    }

    SpanChange ServoChange(int module, int channel, int targetPct, int speed, int accel)
    {
        SpanChange c{};
        c.key = SpanKey{MODULE_TYPE::MAESTRO_RANDOM, module, channel};
        c.isServo = true;
        c.targetPct = targetPct;
        c.speed = speed;
        c.acceleration = accel;
        return c;
    }

    SpanChange ToggleChange(SpanKey key, bool level)
    {
        SpanChange c{};
        c.key = key;
        c.isServo = false;
        c.level = level;
        return c;
    }

    constexpr SpanKey KEY_A{MODULE_TYPE::GPIO_RANDOM, 0, 3};
    constexpr SpanKey KEY_B{MODULE_TYPE::GPIO_RANDOM, 0, 4};
    constexpr SpanKey KEY_C{MODULE_TYPE::MAESTRO_RANDOM, 0, 3};
} // namespace

// ---------------------------------------------------------------- BuildMaestroSpan

TEST(RandomSpanAdmission, ServoChannelBuildsAServoStart)
{
    SpanBuildResult r = BuildMaestroSpan(ServoCommand(), ServoChannel(), 12345);

    ASSERT_EQ(r.reject, SpanReject::NONE);
    EXPECT_FALSE(r.servoFieldsOnDigital);
    ExpectKey(r.start.key, MODULE_TYPE::MAESTRO_RANDOM, 1, 4);
    EXPECT_EQ(r.start.startMs, 12345u);
    EXPECT_EQ(r.start.spanMs, 5000);
    EXPECT_EQ(r.start.minGapMs, 200);
    EXPECT_EQ(r.start.maxGapMs, 800);
    EXPECT_TRUE(r.start.isServo);
    EXPECT_EQ(r.start.minPos, 35);
    EXPECT_EQ(r.start.maxPos, 65);
    EXPECT_EQ(r.start.minMove, 2);
    EXPECT_EQ(r.start.maxMove, 8);
    EXPECT_EQ(r.start.speed, 10);
    EXPECT_EQ(r.start.acceleration, 3);
    EXPECT_EQ(r.start.rangeUs, 1000);
    EXPECT_EQ(r.start.initialPct, 50);
    EXPECT_FALSE(r.start.initialLevel);
}

TEST(RandomSpanAdmission, ServoInitialPercentComesFromLastPosAndUndoesInversion)
{
    MaestroChannelSnapshot ch = ServoChannel();
    ch.lastPos = 1200; // 20 % of 1000-2000

    SpanBuildResult plain = BuildMaestroSpan(ServoCommand(), ch, 0);
    ASSERT_EQ(plain.reject, SpanReject::NONE);
    EXPECT_EQ(plain.start.initialPct, 20);

    ch.inverted = true;
    SpanBuildResult inverted = BuildMaestroSpan(ServoCommand(), ch, 0);
    ASSERT_EQ(inverted.reject, SpanReject::NONE);
    EXPECT_EQ(inverted.start.initialPct, 80);
}

TEST(RandomSpanAdmission, ServoRangeIsTheChannelRangeNotTheSpanRange)
{
    MaestroChannelSnapshot ch = ServoChannel();
    ch.minPos = 600;
    ch.maxPos = 2350;

    SpanBuildResult r = BuildMaestroSpan(ServoCommand(), ch, 0);

    ASSERT_EQ(r.reject, SpanReject::NONE);
    EXPECT_EQ(r.start.rangeUs, 1750);
}

TEST(RandomSpanAdmission, DisabledChannelIsRejected)
{
    MaestroChannelSnapshot servo = ServoChannel();
    servo.enabled = false;
    EXPECT_EQ(BuildMaestroSpan(ServoCommand(), servo, 0).reject, SpanReject::DISABLED_CHANNEL);

    MaestroChannelSnapshot digital = DigitalChannel(500);
    digital.enabled = false;
    EXPECT_EQ(BuildMaestroSpan(DigitalCommand(), digital, 0).reject, SpanReject::DISABLED_CHANNEL);
}

TEST(RandomSpanAdmission, ServoWithNoUsableRangeIsRejected)
{
    MaestroChannelSnapshot ch = ServoChannel();
    ch.minPos = 1500;
    ch.maxPos = 1500;
    EXPECT_EQ(BuildMaestroSpan(ServoCommand(), ch, 0).reject, SpanReject::NO_USABLE_RANGE);

    ch.maxPos = 1400;
    EXPECT_EQ(BuildMaestroSpan(ServoCommand(), ch, 0).reject, SpanReject::NO_USABLE_RANGE);

    ch.maxPos = 1501; // 1 us is a range
    SpanBuildResult r = BuildMaestroSpan(ServoCommand(), ch, 0);
    ASSERT_EQ(r.reject, SpanReject::NONE);
    EXPECT_EQ(r.start.rangeUs, 1);
}

TEST(RandomSpanAdmission, DigitalChannelNeedsNoRange)
{
    // A digital channel's min/max are never used: it toggles 500 / 2500 us.
    MaestroChannelSnapshot ch = DigitalChannel(500);
    ch.minPos = 0;
    ch.maxPos = 0;

    EXPECT_EQ(BuildMaestroSpan(DigitalCommand(), ch, 0).reject, SpanReject::NONE);
}

TEST(RandomSpanAdmission, DigitalSpanForAServoChannelIsAConfigMismatch)
{
    // The server thinks the channel is digital: positions and moves all 0.
    EXPECT_EQ(BuildMaestroSpan(DigitalCommand(), ServoChannel(), 0).reject, SpanReject::DIGITAL_SPAN_ON_SERVO);

    // Only positions and moves decide it; speed and acceleration don't.
    RandomMaestroCommand withSpeed = DigitalCommand(10000, 200, 400);
    withSpeed.speed = 5;
    withSpeed.acceleration = 2;
    EXPECT_EQ(BuildMaestroSpan(withSpeed, ServoChannel(), 0).reject, SpanReject::DIGITAL_SPAN_ON_SERVO);
}

TEST(RandomSpanAdmission, ServoFieldsThatAreNotAllZeroButInvalidAreInvalid)
{
    RandomMaestroCommand onlyMaxPos = DigitalCommand(10000, 200, 400);
    onlyMaxPos.maxPos = 50; // minMove still 0
    EXPECT_EQ(BuildMaestroSpan(onlyMaxPos, ServoChannel(), 0).reject, SpanReject::INVALID_SERVO_FIELDS);

    RandomMaestroCommand onlyMaxMove = DigitalCommand(10000, 200, 400);
    onlyMaxMove.maxMove = 10;
    EXPECT_EQ(BuildMaestroSpan(onlyMaxMove, ServoChannel(), 0).reject, SpanReject::INVALID_SERVO_FIELDS);

    RandomMaestroCommand shortGap = ServoCommand();
    shortGap.minGapMs = 99; // servo minimum is 100
    EXPECT_EQ(BuildMaestroSpan(shortGap, ServoChannel(), 0).reject, SpanReject::INVALID_SERVO_FIELDS);

    RandomMaestroCommand bigMove = ServoCommand();
    bigMove.minMove = 16; // more than half of 35-65
    EXPECT_EQ(BuildMaestroSpan(bigMove, ServoChannel(), 0).reject, SpanReject::INVALID_SERVO_FIELDS);
}

TEST(RandomSpanAdmission, NoUsableRangeIsReportedBeforeTheFieldChecks)
{
    // A channel with no range can't run any servo span, whatever the server sent.
    MaestroChannelSnapshot ch = ServoChannel();
    ch.maxPos = ch.minPos;

    EXPECT_EQ(BuildMaestroSpan(DigitalCommand(), ch, 0).reject, SpanReject::NO_USABLE_RANGE);
}

TEST(RandomSpanAdmission, DigitalChannelBuildsAToggleFromItsLastLevel)
{
    struct Case
    {
        int lastPos;
        bool level;
    };
    const Case cases[] = {{2500, true}, {500, false}, {1500, true}, {1499, false}, {0, false}};

    for (const Case &c : cases)
    {
        SpanBuildResult r = BuildMaestroSpan(DigitalCommand(), DigitalChannel(c.lastPos), 777);
        ASSERT_EQ(r.reject, SpanReject::NONE) << "lastPos " << c.lastPos;
        EXPECT_FALSE(r.servoFieldsOnDigital);
        ExpectKey(r.start.key, MODULE_TYPE::MAESTRO_RANDOM, 2, 7);
        EXPECT_EQ(r.start.startMs, 777u);
        EXPECT_EQ(r.start.spanMs, 10000);
        EXPECT_EQ(r.start.minGapMs, 50);
        EXPECT_EQ(r.start.maxGapMs, 400);
        EXPECT_FALSE(r.start.isServo);
        EXPECT_EQ(r.start.initialLevel, c.level) << "lastPos " << c.lastPos;
        ExpectNoServoFields(r.start);
    }
}

TEST(RandomSpanAdmission, DigitalChannelWithServoFieldsRunsAsAToggleAndFlagsTheMismatch)
{
    for (int field = 0; field < 6; field++)
    {
        RandomMaestroCommand cmd = DigitalCommand();
        switch (field)
        {
        case 0:
            cmd.minPos = 1;
            break;
        case 1:
            cmd.maxPos = 1;
            break;
        case 2:
            cmd.minMove = 1;
            break;
        case 3:
            cmd.maxMove = 1;
            break;
        case 4:
            cmd.speed = 1;
            break;
        default:
            cmd.acceleration = 1;
            break;
        }

        SpanBuildResult r = BuildMaestroSpan(cmd, DigitalChannel(2500), 0);
        ASSERT_EQ(r.reject, SpanReject::NONE) << "field " << field;
        EXPECT_TRUE(r.servoFieldsOnDigital) << "field " << field;
        EXPECT_FALSE(r.start.isServo);
        EXPECT_TRUE(r.start.initialLevel);
        ExpectNoServoFields(r.start); // the toggle never sees the stray fields
    }
}

TEST(RandomSpanAdmission, MaestroSpanWithinTheEndGuardIsRejected)
{
    // 20 ms (the minimum gap) is the only valid span the guard catches.
    EXPECT_EQ(BuildMaestroSpan(DigitalCommand(20, 20, 20), DigitalChannel(500), 0).reject,
              SpanReject::WITHIN_END_GUARD);
    EXPECT_EQ(BuildMaestroSpan(DigitalCommand(21, 20, 20), DigitalChannel(500), 0).reject, SpanReject::NONE);
}

// ---------------------------------------------------------------- BuildGpioSpan

TEST(RandomSpanAdmission, GpioBuildsAToggleWithModuleZero)
{
    SpanBuildResult high = BuildGpioSpan(GpioCommandFor(3), true, 4242);

    ASSERT_EQ(high.reject, SpanReject::NONE);
    EXPECT_FALSE(high.servoFieldsOnDigital);
    ExpectKey(high.start.key, MODULE_TYPE::GPIO_RANDOM, 0, 3);
    EXPECT_EQ(high.start.startMs, 4242u);
    EXPECT_EQ(high.start.spanMs, 10000);
    EXPECT_EQ(high.start.minGapMs, 50);
    EXPECT_EQ(high.start.maxGapMs, 400);
    EXPECT_FALSE(high.start.isServo);
    EXPECT_TRUE(high.start.initialLevel);
    ExpectNoServoFields(high.start);

    SpanBuildResult low = BuildGpioSpan(GpioCommandFor(3), false, 4242);
    ASSERT_EQ(low.reject, SpanReject::NONE);
    EXPECT_FALSE(low.start.initialLevel);
}

TEST(RandomSpanAdmission, GpioSpanWithinTheEndGuardIsRejected)
{
    EXPECT_EQ(BuildGpioSpan(GpioCommandFor(3, 20, 20, 20), false, 0).reject, SpanReject::WITHIN_END_GUARD);
    EXPECT_EQ(BuildGpioSpan(GpioCommandFor(3, 21, 20, 20), false, 0).reject, SpanReject::NONE);
}

// ---------------------------------------------------------------- SpanArrivedTooLate

TEST(RandomSpanAdmission, SpanArrivesTooLateOnceNowReachesItsEndGuard)
{
    // Dispatched at 1000 for 5000 ms: the last change may run before 5980.
    EXPECT_FALSE(SpanArrivedTooLate(1000, 5000, 1000));
    EXPECT_FALSE(SpanArrivedTooLate(1000, 5000, 5979));
    EXPECT_TRUE(SpanArrivedTooLate(1000, 5000, 5980));
    EXPECT_TRUE(SpanArrivedTooLate(1000, 5000, 6000));
    EXPECT_TRUE(SpanArrivedTooLate(1000, 5000, 9000));
}

TEST(RandomSpanAdmission, SpanArrivedTooLateIsWrapSafe)
{
    // 0xFFFFF000 + 5000 - 20 wraps to 884.
    const uint32_t dispatch = 0xFFFFF000u;
    EXPECT_FALSE(SpanArrivedTooLate(dispatch, 5000, dispatch));
    EXPECT_FALSE(SpanArrivedTooLate(dispatch, 5000, 0xFFFFFFFFu));
    EXPECT_FALSE(SpanArrivedTooLate(dispatch, 5000, 883));
    EXPECT_TRUE(SpanArrivedTooLate(dispatch, 5000, 884));
}

// ---------------------------------------------------------------- ClearFence

TEST(RandomSpanAdmission, FenceStopsNothingBeforeTheFirstClear)
{
    ClearFence fence;
    fence.Expire(0);

    EXPECT_FALSE(fence.Fences(0));
    EXPECT_FALSE(fence.Fences(5000));
}

TEST(RandomSpanAdmission, FenceStopsSpansDispatchedAtOrBeforeTheClear)
{
    ClearFence fence;
    fence.Mark(10000);

    EXPECT_TRUE(fence.Fences(9000));
    EXPECT_TRUE(fence.Fences(9999));
    EXPECT_TRUE(fence.Fences(10000));
    EXPECT_FALSE(fence.Fences(10001));
}

TEST(RandomSpanAdmission, FenceUsesTheLatestClear)
{
    ClearFence fence;
    fence.Mark(1000);
    fence.Mark(5000);

    EXPECT_TRUE(fence.Fences(3000));
    EXPECT_FALSE(fence.Fences(5001));
}

TEST(RandomSpanAdmission, FenceIsWrapSafe)
{
    ClearFence fence;
    fence.Mark(0xFFFFFF00u);

    EXPECT_TRUE(fence.Fences(0xFFFFFE00u));
    EXPECT_FALSE(fence.Fences(0x00000010u)); // after the wrap
}

TEST(RandomSpanAdmission, FenceKeepsARecentClear)
{
    ClearFence fence;
    fence.Mark(1000);
    fence.Expire(1000 + ClearFence::FORGET_AFTER_MS - 1);

    EXPECT_TRUE(fence.Fences(1000));
}

TEST(RandomSpanAdmission, FenceForgetsAnOldClearSoUptimeNeverRejectsEverySpan)
{
    // Without forgetting, a span dispatched 2^31 ms after the clear compares as
    // "before" it (the int32 difference goes negative) and is rejected.
    ClearFence fence;
    fence.Mark(1000);
    fence.Expire(1000 + ClearFence::FORGET_AFTER_MS);

    EXPECT_FALSE(fence.Fences(1000));
    EXPECT_FALSE(fence.Fences(1000 + (1u << 31) + 10));
}

TEST(RandomSpanAdmission, FenceMarkedAgainAfterForgettingFencesAgain)
{
    ClearFence fence;
    fence.Mark(1000);
    fence.Expire(1000 + ClearFence::FORGET_AFTER_MS);
    fence.Mark(2000000000u);

    EXPECT_TRUE(fence.Fences(1999999999u));
}

// ---------------------------------------------------------------- FormatSpanChange

TEST(RandomSpanAdmission, ServoChangeFormatsAsAType1Command)
{
    EXPECT_EQ(Format(ServoChange(2, 11, 47, 10, 3)), "1|0|2|11|47|10|3");
    EXPECT_EQ(Format(ServoChange(0, 0, 0, 0, 0)), "1|0|0|0|0|0|0");
    EXPECT_EQ(Format(ServoChange(0, 23, 100, 255, 255)), "1|0|0|23|100|255|255");
}

TEST(RandomSpanAdmission, DigitalChangeFormatsAsAType1CommandAt2500Or500)
{
    const SpanKey key{MODULE_TYPE::MAESTRO_RANDOM, 1, 4};
    EXPECT_EQ(Format(ToggleChange(key, true)), "1|0|1|4|2500|0|0");
    EXPECT_EQ(Format(ToggleChange(key, false)), "1|0|1|4|500|0|0");
}

TEST(RandomSpanAdmission, GpioChangeFormatsAsAType5Command)
{
    const SpanKey key{MODULE_TYPE::GPIO_RANDOM, 0, 3};
    EXPECT_EQ(Format(ToggleChange(key, true)), "5|0|3|1");
    EXPECT_EQ(Format(ToggleChange(key, false)), "5|0|3|0");
}

TEST(RandomSpanAdmission, ServoChangeParsesBackThroughMaestroCommand)
{
    MaestroCommand parsed(Format(ServoChange(2, 11, 47, 10, 3)));

    EXPECT_EQ(parsed.controller, "2");
    EXPECT_EQ(parsed.channel, 11);
    EXPECT_EQ(parsed.position, 47);
    EXPECT_EQ(parsed.speed, 10);
    EXPECT_EQ(parsed.acceleration, 3);
}

TEST(RandomSpanAdmission, DigitalChangeParsesBackThroughMaestroCommand)
{
    MaestroCommand parsed(Format(ToggleChange(SpanKey{MODULE_TYPE::MAESTRO_RANDOM, 1, 4}, true)));

    EXPECT_EQ(parsed.channel, 4);
    EXPECT_EQ(parsed.position, 2500);
    EXPECT_EQ(parsed.speed, 0);
    EXPECT_EQ(parsed.acceleration, 0);
}

TEST(RandomSpanAdmission, GpioChangeParsesBackThroughGpioCommand)
{
    GpioCommand high(Format(ToggleChange(SpanKey{MODULE_TYPE::GPIO_RANDOM, 0, 9}, true)));
    EXPECT_EQ(high.channel, 9);
    EXPECT_TRUE(high.state);

    GpioCommand low(Format(ToggleChange(SpanKey{MODULE_TYPE::GPIO_RANDOM, 0, 9}, false)));
    EXPECT_EQ(low.channel, 9);
    EXPECT_FALSE(low.state);
}

TEST(RandomSpanAdmission, FormatReturnsZeroForAKeyThatIsNotASpanType)
{
    SpanChange c = ToggleChange(SpanKey{MODULE_TYPE::GPIO, 0, 3}, true);
    char buf[SPAN_CHANGE_MAX_LEN];

    EXPECT_EQ(FormatSpanChange(c, buf, sizeof(buf)), 0u);
}

TEST(RandomSpanAdmission, FormatReturnsZeroRatherThanTruncating)
{
    SpanChange c = ToggleChange(SpanKey{MODULE_TYPE::GPIO_RANDOM, 0, 3}, true); // "5|0|3|1", 7 chars
    char buf[8];

    EXPECT_EQ(FormatSpanChange(c, buf, 7), 0u);
    EXPECT_EQ(FormatSpanChange(c, buf, 0), 0u);
    ASSERT_EQ(FormatSpanChange(c, buf, 8), 7u);
    EXPECT_STREQ(buf, "5|0|3|1");
}

TEST(RandomSpanAdmission, LongestChangeFitsTheMaxLength)
{
    SpanChange c = ServoChange(INT_MIN, INT_MIN, INT_MIN, INT_MIN, INT_MIN);
    char buf[SPAN_CHANGE_MAX_LEN];

    size_t len = FormatSpanChange(c, buf, sizeof(buf));

    EXPECT_EQ(len, std::strlen("1|0|-2147483648|-2147483648|-2147483648|-2147483648|-2147483648"));
}

// ---------------------------------------------------------------- DropLogThrottle

TEST(RandomSpanAdmission, ThrottleLogsTheFirstDropOfASpanOnly)
{
    DropLogThrottle throttle(4);

    EXPECT_TRUE(throttle.ShouldLog(KEY_A));
    EXPECT_FALSE(throttle.ShouldLog(KEY_A));
    EXPECT_FALSE(throttle.ShouldLog(KEY_A));
}

TEST(RandomSpanAdmission, ThrottleKeysDifferInTypeModuleAndChannel)
{
    DropLogThrottle throttle(8);
    ASSERT_TRUE(throttle.ShouldLog(KEY_A));

    EXPECT_TRUE(throttle.ShouldLog(KEY_B));                                      // channel
    EXPECT_TRUE(throttle.ShouldLog(KEY_C));                                      // type
    EXPECT_TRUE(throttle.ShouldLog(SpanKey{MODULE_TYPE::MAESTRO_RANDOM, 1, 3})); // module
    EXPECT_FALSE(throttle.ShouldLog(KEY_A));
}

TEST(RandomSpanAdmission, ThrottleResetLogsTheNextDropAgain)
{
    DropLogThrottle throttle(4);
    ASSERT_TRUE(throttle.ShouldLog(KEY_A));
    ASSERT_TRUE(throttle.ShouldLog(KEY_B));

    throttle.Reset(KEY_A);

    EXPECT_TRUE(throttle.ShouldLog(KEY_A));
    EXPECT_FALSE(throttle.ShouldLog(KEY_B)); // only the reset key
}

TEST(RandomSpanAdmission, ThrottleResetOfAnUnknownKeyChangesNothing)
{
    DropLogThrottle throttle(4);
    ASSERT_TRUE(throttle.ShouldLog(KEY_A));

    throttle.Reset(KEY_B);

    EXPECT_FALSE(throttle.ShouldLog(KEY_A));
    EXPECT_TRUE(throttle.ShouldLog(KEY_B));
}

TEST(RandomSpanAdmission, FullThrottleStillLogsANewKeyAndEvictsTheOldest)
{
    DropLogThrottle throttle(2);
    ASSERT_TRUE(throttle.ShouldLog(KEY_A));
    ASSERT_TRUE(throttle.ShouldLog(KEY_B));

    EXPECT_TRUE(throttle.ShouldLog(KEY_C));  // a full throttle never hides a new span's first drop
    EXPECT_FALSE(throttle.ShouldLog(KEY_C)); // and remembers it
    EXPECT_FALSE(throttle.ShouldLog(KEY_B)); // the newer of the old keys survives
    EXPECT_TRUE(throttle.ShouldLog(KEY_A));  // the evicted one logs again: one extra line, no more
}

TEST(RandomSpanAdmission, ZeroCapacityThrottleLogsEveryDrop)
{
    DropLogThrottle throttle(0);

    EXPECT_TRUE(throttle.ShouldLog(KEY_A));
    EXPECT_TRUE(throttle.ShouldLog(KEY_A));
}

// ---------------------------------------------------------------- WaitTicksCeil

TEST(RandomSpanAdmission, WaitTicksRoundUp)
{
    // At 100 Hz (10 ms ticks) a 1-9 ms wait must not become 0 ticks.
    EXPECT_EQ(WaitTicksCeil(0, 10), 0u);
    EXPECT_EQ(WaitTicksCeil(1, 10), 1u);
    EXPECT_EQ(WaitTicksCeil(9, 10), 1u);
    EXPECT_EQ(WaitTicksCeil(10, 10), 1u);
    EXPECT_EQ(WaitTicksCeil(11, 10), 2u);
    EXPECT_EQ(WaitTicksCeil(250, 10), 25u);
    EXPECT_EQ(WaitTicksCeil(7, 1), 7u);
}

TEST(RandomSpanAdmission, WaitTicksDoNotOverflow)
{
    EXPECT_EQ(WaitTicksCeil(UINT32_MAX, 10), 429496730u);
    EXPECT_EQ(WaitTicksCeil(UINT32_MAX, 1), UINT32_MAX);
}

// ---------------------------------------------------------------- RandomChangeHasRoom

TEST(RandomSpanAdmission, RandomChangesFillAtMostHalfOfAChangeQueue)
{
    // servoQueue is 20 deep: random changes stop at 10 queued, leaving 10 for the script.
    EXPECT_TRUE(RandomChangeHasRoom(0, 20));
    EXPECT_TRUE(RandomChangeHasRoom(9, 11));
    EXPECT_FALSE(RandomChangeHasRoom(10, 10));
    EXPECT_FALSE(RandomChangeHasRoom(11, 9));
    EXPECT_FALSE(RandomChangeHasRoom(20, 0));

    // gpioQueue is 10 deep: random changes stop at 5.
    EXPECT_TRUE(RandomChangeHasRoom(4, 6));
    EXPECT_FALSE(RandomChangeHasRoom(5, 5));
}

TEST(RandomSpanAdmission, RandomChangeRoomOnOddAndEmptyQueues)
{
    EXPECT_TRUE(RandomChangeHasRoom(2, 3)); // 5 deep: up to 3 random
    EXPECT_FALSE(RandomChangeHasRoom(3, 2));
    EXPECT_FALSE(RandomChangeHasRoom(0, 0));
}

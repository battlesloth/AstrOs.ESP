#include <AstrOsEnums.h>
#include <AstrOsServoUtils.hpp>
#include <RandomEffects.hpp>
#include <gtest/gtest.h>

#include <climits>
#include <cstdlib>
#include <deque>
#include <random>
#include <set>
#include <vector>

// Move rule and span semantics: .docs/protocol.md § C and
// .docs/tasks/T-008-random-effects-math.md.

namespace
{
    // Returns the queued draws in order; running out is a test failure (the code
    // drew more than the test expected). UniformInt(lo, hi) takes draw % (hi - lo + 1),
    // so a draw of n picks lo + n.
    Rand32 Scripted(std::deque<uint32_t> &draws)
    {
        return [&draws]() -> uint32_t
        {
            if (draws.empty())
            {
                ADD_FAILURE() << "Rand32 drawn more often than the test scripted";
                return 0;
            }
            uint32_t value = draws.front();
            draws.pop_front();
            return value;
        };
    }

    // Fails the test if it is ever drawn.
    Rand32 NoDraws()
    {
        return []() -> uint32_t
        {
            ADD_FAILURE() << "Rand32 drawn when no draw was expected";
            return 0;
        };
    }

    constexpr SpanKey GPIO_KEY{MODULE_TYPE::GPIO_RANDOM, 0, 4};
    constexpr SpanKey MAESTRO_KEY{MODULE_TYPE::MAESTRO_RANDOM, 0, 4};

    // A toggle span with a fixed gap (minGap == maxGap, so no gap draws).
    SpanStart Toggle(SpanKey key, uint32_t startMs, int spanMs, int gapMs, bool initialLevel)
    {
        SpanStart s{};
        s.key = key;
        s.startMs = startMs;
        s.spanMs = spanMs;
        s.minGapMs = gapMs;
        s.maxGapMs = gapMs;
        s.isServo = false;
        s.initialLevel = initialLevel;
        return s;
    }

    // A servo span with a fixed gap, speed/accel 0 and a 2000 us channel.
    SpanStart Servo(SpanKey key, uint32_t startMs, int spanMs, int gapMs, int minPos, int maxPos, int minMove,
                    int maxMove, int initialPct)
    {
        SpanStart s{};
        s.key = key;
        s.startMs = startMs;
        s.spanMs = spanMs;
        s.minGapMs = gapMs;
        s.maxGapMs = gapMs;
        s.isServo = true;
        s.minPos = minPos;
        s.maxPos = maxPos;
        s.minMove = minMove;
        s.maxMove = maxMove;
        s.speed = 0;
        s.acceleration = 0;
        s.rangeUs = 2000;
        s.initialPct = initialPct;
        return s;
    }

    bool SameKey(const SpanKey &a, const SpanKey &b)
    {
        return a.type == b.type && a.module == b.module && a.channel == b.channel;
    }

    std::vector<SpanChange> CollectAt(RandomSpanTable &table, uint32_t nowMs)
    {
        std::vector<SpanChange> out;
        table.Collect(nowMs, out);
        return out;
    }
} // namespace

// ---- UniformInt / NextGapMs ----

TEST(RandomEffects, UniformIntBoundsInclusive)
{
    std::deque<uint32_t> draws{0, 4, 5, 9};
    Rand32 rand = Scripted(draws);
    EXPECT_EQ(3, UniformInt(3, 7, rand));
    EXPECT_EQ(7, UniformInt(3, 7, rand));
    EXPECT_EQ(3, UniformInt(3, 7, rand)); // 5 values: draw 5 wraps to lo
    EXPECT_EQ(7, UniformInt(3, 7, rand));

    std::deque<uint32_t> negative{0, 10};
    rand = Scripted(negative);
    EXPECT_EQ(-5, UniformInt(-5, 5, rand));
    EXPECT_EQ(5, UniformInt(-5, 5, rand));

    // The full int range must not overflow (2^32 values).
    std::deque<uint32_t> wide{0, UINT32_MAX};
    rand = Scripted(wide);
    EXPECT_EQ(INT_MIN, UniformInt(INT_MIN, INT_MAX, rand));
    EXPECT_EQ(INT_MAX, UniformInt(INT_MIN, INT_MAX, rand));

    std::mt19937 gen(12345);
    Rand32 seeded = [&gen]() -> uint32_t { return gen(); };
    std::set<int> seen;
    for (int i = 0; i < 10000; i++)
    {
        int v = UniformInt(3, 7, seeded);
        ASSERT_GE(v, 3);
        ASSERT_LE(v, 7);
        seen.insert(v);
    }
    EXPECT_EQ(5u, seen.size());
}

// Ruling: an empty or inverted range returns lo without drawing, so scripted
// tests stay in step and fixed gaps cost no RNG call.
TEST(RandomEffects, UniformIntLoEqualsHi)
{
    Rand32 rand = NoDraws();
    EXPECT_EQ(4, UniformInt(4, 4, rand));
    EXPECT_EQ(9, UniformInt(9, 2, rand));
}

TEST(RandomEffects, NextGapMsDrawsInclusiveRange)
{
    std::deque<uint32_t> draws{0, 300, 301};
    Rand32 rand = Scripted(draws);
    EXPECT_EQ(100, NextGapMs(100, 400, rand));
    EXPECT_EQ(400, NextGapMs(100, 400, rand));
    EXPECT_EQ(100, NextGapMs(100, 400, rand));

    EXPECT_EQ(250, NextGapMs(250, 250, NoDraws()));
}

// ---- NextServoTarget ----
// Range 20-100, move 15-60 unless stated: up is forced at <= 35, down at >= 85.

TEST(RandomEffects, NextServoTargetForcedUpNearMin)
{
    // Forced: no direction draw, only the size draw (size = 15 + draw).
    std::deque<uint32_t> draws{0, 45, 0};
    Rand32 rand = Scripted(draws);
    EXPECT_EQ(35, NextServoTarget(20, 20, 100, 15, 60, rand)); // current = minPos
    EXPECT_EQ(80, NextServoTarget(20, 20, 100, 15, 60, rand)); // size 60
    EXPECT_EQ(50, NextServoTarget(35, 20, 100, 15, 60, rand)); // current = minPos + minMove
    EXPECT_TRUE(draws.empty());

    std::mt19937 gen(7);
    Rand32 seeded = [&gen]() -> uint32_t { return gen(); };
    for (int current : {20, 35})
    {
        for (int i = 0; i < 1000; i++)
        {
            int next = NextServoTarget(current, 20, 100, 15, 60, seeded);
            ASSERT_GE(next - current, 15) << "from " << current;
        }
    }
}

TEST(RandomEffects, NextServoTargetForcedDownNearMax)
{
    std::deque<uint32_t> draws{0, 45, 0};
    Rand32 rand = Scripted(draws);
    EXPECT_EQ(85, NextServoTarget(100, 20, 100, 15, 60, rand)); // current = maxPos
    EXPECT_EQ(40, NextServoTarget(100, 20, 100, 15, 60, rand)); // size 60
    EXPECT_EQ(70, NextServoTarget(85, 20, 100, 15, 60, rand));  // current = maxPos - minMove
    EXPECT_TRUE(draws.empty());

    std::mt19937 gen(8);
    Rand32 seeded = [&gen]() -> uint32_t { return gen(); };
    for (int current : {100, 85})
    {
        for (int i = 0; i < 1000; i++)
        {
            int next = NextServoTarget(current, 20, 100, 15, 60, seeded);
            ASSERT_GE(current - next, 15) << "from " << current;
        }
    }
}

TEST(RandomEffects, NextServoTargetMiddleUsesRandomDirection)
{
    // 60 is in the middle (36..84): the first draw picks the direction by parity.
    std::deque<uint32_t> draws{1, 0, 0, 0, 3, 0, 2, 0};
    Rand32 rand = Scripted(draws);
    int odd = NextServoTarget(60, 20, 100, 15, 60, rand);
    int even = NextServoTarget(60, 20, 100, 15, 60, rand);
    EXPECT_EQ(15, std::abs(odd - 60));
    EXPECT_EQ(15, std::abs(even - 60));
    EXPECT_NE(odd, even);

    // Parity, not magnitude: 3 goes the way 1 did, 2 the way 0 did.
    EXPECT_EQ(odd, NextServoTarget(60, 20, 100, 15, 60, rand));
    EXPECT_EQ(even, NextServoTarget(60, 20, 100, 15, 60, rand));
    EXPECT_TRUE(draws.empty());
}

TEST(RandomEffects, NextServoTargetNoMaxUsesRoom)
{
    // Range 0-100, move 5-none, current 30 (middle). Find which parity goes down.
    std::deque<uint32_t> probe{0, 0};
    Rand32 rand = Scripted(probe);
    uint32_t down = NextServoTarget(30, 0, 100, 5, 0, rand) < 30 ? 0 : 1;
    uint32_t up = 1 - down;

    // Down: room 30, so the size is 5..30 (26 values).
    std::deque<uint32_t> draws{down, 25, down, 26, up, 65, up, 66};
    rand = Scripted(draws);
    EXPECT_EQ(0, NextServoTarget(30, 0, 100, 5, 0, rand));   // size 30 = the room
    EXPECT_EQ(25, NextServoTarget(30, 0, 100, 5, 0, rand));  // 26 wraps to size 5
    EXPECT_EQ(100, NextServoTarget(30, 0, 100, 5, 0, rand)); // up: room 70, size 5..70
    EXPECT_EQ(35, NextServoTarget(30, 0, 100, 5, 0, rand));  // 66 wraps to size 5
    EXPECT_TRUE(draws.empty());
}

TEST(RandomEffects, NextServoTargetMaxMoveCapsSize)
{
    // Forced up from 21: room 79, but maxMove 60 caps the size at 15..60 (46 values).
    std::deque<uint32_t> draws{45, 46};
    Rand32 rand = Scripted(draws);
    EXPECT_EQ(81, NextServoTarget(21, 20, 100, 15, 60, rand));
    EXPECT_EQ(36, NextServoTarget(21, 20, 100, 15, 60, rand));
    EXPECT_TRUE(draws.empty());
}

TEST(RandomEffects, NextServoTargetOutsideRangeJumpsInside)
{
    // One draw, uniform over 20..100 (81 values); no direction draw, no minMove.
    std::deque<uint32_t> draws{0, 80, 81, 80};
    Rand32 rand = Scripted(draws);
    EXPECT_EQ(20, NextServoTarget(0, 20, 100, 15, 60, rand));
    EXPECT_EQ(100, NextServoTarget(0, 20, 100, 15, 60, rand));
    EXPECT_EQ(20, NextServoTarget(0, 20, 100, 15, 60, rand));
    EXPECT_EQ(100, NextServoTarget(110, 20, 100, 15, 60, rand)); // a 10 move, under minMove
    EXPECT_TRUE(draws.empty());
}

// Jeff's rule: a move that would pass a limit stops at it. With § C-valid fields
// the size never exceeds the room; this needs minMove > range / 2, which makes
// every in-range position forced up (the up check runs first, so a downward
// move always has more than minMove of room).
TEST(RandomEffects, NextServoTargetClampsToLimit)
{
    Rand32 rand = NoDraws();
    EXPECT_EQ(60, NextServoTarget(50, 40, 60, 30, 0, rand)); // size 30 > room 10
    EXPECT_EQ(60, NextServoTarget(40, 40, 60, 30, 0, rand)); // size 30 > room 20
}

TEST(RandomEffects, NextServoTargetPropertyHolds)
{
    struct Config
    {
        int minPos, maxPos, minMove, maxMove;
    };
    const Config configs[] = {{20, 100, 15, 60}, {35, 65, 2, 8}, {0, 100, 5, 0}, {40, 60, 10, 0}};

    for (const Config &c : configs)
    {
        std::mt19937 gen(20261022);
        Rand32 seeded = [&gen]() -> uint32_t { return gen(); };
        int current = (c.minPos + c.maxPos) / 2;
        for (int i = 0; i < 10000; i++)
        {
            int next = NextServoTarget(current, c.minPos, c.maxPos, c.minMove, c.maxMove, seeded);
            int move = std::abs(next - current);
            ASSERT_GE(next, c.minPos) << "config " << c.minPos << "-" << c.maxPos << " step " << i;
            ASSERT_LE(next, c.maxPos) << "config " << c.minPos << "-" << c.maxPos << " step " << i;
            ASSERT_GE(move, c.minMove) << "config " << c.minPos << "-" << c.maxPos << " step " << i;
            if (c.maxMove != 0)
            {
                ASSERT_LE(move, c.maxMove) << "config " << c.minPos << "-" << c.maxPos << " step " << i;
            }
            current = next;
        }
    }
}

// ---- MoveTravelMs ----

// Same Maestro model as WorstCaseTravelMs, at the move's distance d:
//   cruise   ceil(40 d / cap)                       (cap = speed, 0 -> 255)
//   trapezoid cruise + ceil(80 cap / accel)         when 2 cap^2 <= d accel
//   triangle ceilSqrt(ceil(12800 d / accel))        otherwise
TEST(RandomEffects, MoveTravelMsMatchesWorstCaseAtFullRange)
{
    for (int speed = 0; speed <= 255; speed++)
    {
        for (int accel = 0; accel <= 255; accel++)
        {
            ASSERT_EQ(WorstCaseTravelMs(speed, accel), MoveTravelMs(3000, speed, accel)) << speed << "/" << accel;
        }
    }

    // Out-of-range inputs clamp to 0-255 the same way.
    const int outOfRange[][2] = {{-5, 0}, {300, 0}, {20, -5}, {20, 300}, {300, 300}};
    for (const auto &c : outOfRange)
    {
        EXPECT_EQ(WorstCaseTravelMs(c[0], c[1]), MoveTravelMs(3000, c[0], c[1])) << c[0] << "/" << c[1];
    }

    EXPECT_EQ(0, MoveTravelMs(0, 1, 0));
    EXPECT_EQ(0, MoveTravelMs(-100, 1, 0));
}

TEST(RandomEffects, MoveTravelMsAccelZeroIsLinear)
{
    EXPECT_EQ(60000, MoveTravelMs(1500, 1, 0)); // exact half of 120000
    EXPECT_EQ(236, MoveTravelMs(1500, 0, 0));   // ceil(60000 / 255)
    EXPECT_EQ(1, MoveTravelMs(1, 0, 0));        // ceil(40 / 255)
}

// The reason MoveTravelMs isn't WorstCaseTravelMs * d / 3000: a move that never
// reaches the speed cap takes sqrt-of-distance time.
TEST(RandomEffects, MoveTravelMsTriangleGrowsWithSqrtDistance)
{
    EXPECT_EQ(716, MoveTravelMs(200, 0, 5)); // linear scaling would give 185
}

TEST(RandomEffects, MoveTravelMsTrapezoidKeepsFullRamps)
{
    EXPECT_EQ(3800, MoveTravelMs(1500, 20, 2)); // 3000 cruise + 800 ramps; linear would give 3400
}

TEST(RandomEffects, MoveTravelMsRegimeBoundaryIsContinuous)
{
    // Speed 10, accel 1: the cap is reached exactly at 200 us (2 * 10^2 == 200 * 1).
    EXPECT_EQ(1596, MoveTravelMs(199, 10, 1)); // triangle
    EXPECT_EQ(1600, MoveTravelMs(200, 10, 1)); // trapezoid: 800 + 800
    EXPECT_EQ(1604, MoveTravelMs(201, 10, 1)); // trapezoid: 804 + 800
}

// Ruling: travel saturates at INT_MAX instead of overflowing (rangeUs comes from
// channel config, so the product isn't bounded by § C).
TEST(RandomEffects, MoveTravelMsSaturates)
{
    EXPECT_EQ(INT_MAX, MoveTravelMs(INT_MAX, 1, 0));
}

// ---- PercentFromPosition ----

TEST(RandomEffects, PercentFromPositionRoundTrips)
{
    for (int p = 0; p <= 100; p++)
    {
        int us = GetRelativeRequestedPosition(500, 2500, p);
        EXPECT_NEAR(p, PercentFromPosition(500, 2500, us, false), 1) << "p " << p;
    }

    EXPECT_EQ(0, PercentFromPosition(500, 2500, 500, false));
    EXPECT_EQ(50, PercentFromPosition(500, 2500, 1500, false));
    EXPECT_EQ(100, PercentFromPosition(500, 2500, 2500, false));
    EXPECT_EQ(0, PercentFromPosition(500, 2500, 509, false));    // 0.45 rounds down
    EXPECT_EQ(1, PercentFromPosition(500, 2500, 511, false));    // 0.55 rounds up
    EXPECT_EQ(0, PercentFromPosition(500, 2500, 100, false));    // below min clamps
    EXPECT_EQ(100, PercentFromPosition(500, 2500, 3000, false)); // above max clamps
}

TEST(RandomEffects, PercentFromPositionInverted)
{
    EXPECT_EQ(100, PercentFromPosition(500, 2500, 500, true));
    EXPECT_EQ(0, PercentFromPosition(500, 2500, 2500, true));
    EXPECT_EQ(75, PercentFromPosition(500, 2500, 1000, true));
    EXPECT_EQ(100, PercentFromPosition(500, 2500, 100, true)); // clamps before inverting

    // MaestroModule::QueueCommand inverts a script percent as 100 - p before
    // GetRelativeRequestedPosition; PercentFromPosition must undo exactly that.
    for (int p = 0; p <= 100; p++)
    {
        int us = GetRelativeRequestedPosition(500, 2500, 100 - p);
        EXPECT_NEAR(p, PercentFromPosition(500, 2500, us, true), 1) << "p " << p;
    }
}

// Ruling: a channel with maxUs <= minUs has no percent scale; it reads as 0
// (100 when inverted) instead of dividing by zero.
TEST(RandomEffects, PercentFromPositionDegenerateRange)
{
    EXPECT_EQ(0, PercentFromPosition(1500, 1500, 1500, false));
    EXPECT_EQ(100, PercentFromPosition(1500, 1500, 1500, true));
    // Off the single point too: without the guard these divide by zero to +-inf.
    EXPECT_EQ(0, PercentFromPosition(1500, 1500, 1600, false));
    EXPECT_EQ(0, PercentFromPosition(1500, 1500, 1400, false));
    EXPECT_EQ(0, PercentFromPosition(2000, 1000, 1500, false));
}

// ---- RandomSpanTable ----

TEST(RandomEffects, TableFirstChangeAtStart)
{
    RandomSpanTable table(4, NoDraws());
    ASSERT_TRUE(table.Start(Toggle(GPIO_KEY, 1000, 5000, 100, false)));

    EXPECT_TRUE(CollectAt(table, 999).empty());
    auto changes = CollectAt(table, 1000);
    ASSERT_EQ(1u, changes.size());
    EXPECT_TRUE(SameKey(GPIO_KEY, changes[0].key));
    EXPECT_FALSE(changes[0].isServo);
    EXPECT_TRUE(changes[0].level);
    EXPECT_TRUE(CollectAt(table, 1000).empty()); // next is 1100
}

TEST(RandomEffects, TableServoChangeCarriesTargetAndMotion)
{
    std::deque<uint32_t> draws{0};
    RandomSpanTable table(4, Scripted(draws));
    SpanStart s = Servo(MAESTRO_KEY, 1000, 5000, 400, 20, 100, 15, 60, 20);
    s.speed = 12;
    s.acceleration = 3;
    ASSERT_TRUE(table.Start(s));

    auto changes = CollectAt(table, 1000);
    ASSERT_EQ(1u, changes.size());
    EXPECT_TRUE(SameKey(MAESTRO_KEY, changes[0].key));
    EXPECT_TRUE(changes[0].isServo);
    EXPECT_EQ(35, changes[0].targetPct); // forced up from 20 by 15
    EXPECT_EQ(12, changes[0].speed);
    EXPECT_EQ(3, changes[0].acceleration);
}

TEST(RandomEffects, TableServoMovesFromLastTarget)
{
    // Range 0-100, move 10-none: 50 and 60 are both in the middle.
    std::deque<uint32_t> probe{0, 0};
    Rand32 probeRand = Scripted(probe);
    uint32_t up = NextServoTarget(50, 0, 100, 10, 0, probeRand) > 50 ? 0 : 1;
    uint32_t down = 1 - up;

    std::deque<uint32_t> draws{up, 0, down, 0};
    RandomSpanTable table(4, Scripted(draws));
    ASSERT_TRUE(table.Start(Servo(MAESTRO_KEY, 1000, 5000, 400, 0, 100, 10, 0, 50)));

    auto first = CollectAt(table, 1000);
    ASSERT_EQ(1u, first.size());
    EXPECT_EQ(60, first[0].targetPct);
    auto second = CollectAt(table, 1400);
    ASSERT_EQ(1u, second.size());
    EXPECT_EQ(50, second[0].targetPct); // down 10 from 60, not from the initial 50
}

TEST(RandomEffects, TableToggleAlternatesFromInitialLevel)
{
    RandomSpanTable table(4, NoDraws());
    ASSERT_TRUE(table.Start(Toggle(GPIO_KEY, 1000, 5000, 100, false)));
    SpanKey other{MODULE_TYPE::GPIO_RANDOM, 0, 5};
    ASSERT_TRUE(table.Start(Toggle(other, 1000, 5000, 100, true)));

    const bool expectedFromLow[] = {true, false, true, false};
    for (int i = 0; i < 4; i++)
    {
        auto changes = CollectAt(table, 1000 + i * 100);
        ASSERT_EQ(2u, changes.size()) << "step " << i;
        for (const SpanChange &c : changes)
        {
            bool fromLow = SameKey(GPIO_KEY, c.key);
            EXPECT_EQ(fromLow ? expectedFromLow[i] : !expectedFromLow[i], c.level) << "step " << i;
        }
    }
}

TEST(RandomEffects, TableReplacesSameKey)
{
    RandomSpanTable table(4, NoDraws());
    ASSERT_TRUE(table.Start(Toggle(GPIO_KEY, 1000, 5000, 100, false)));
    ASSERT_EQ(1u, CollectAt(table, 1000).size());

    // Same key: replaced in place, with the new start, gap and level.
    ASSERT_TRUE(table.Start(Toggle(GPIO_KEY, 1050, 5000, 200, true)));
    EXPECT_EQ(1u, table.ActiveCount());
    auto changes = CollectAt(table, 1050);
    ASSERT_EQ(1u, changes.size());
    EXPECT_FALSE(changes[0].level);              // toggled from the new initial level
    EXPECT_TRUE(CollectAt(table, 1100).empty()); // the old schedule is gone
    EXPECT_EQ(1u, CollectAt(table, 1250).size());

    // A key differs if any of type, module or channel differs.
    ASSERT_TRUE(table.Start(Toggle(MAESTRO_KEY, 1000, 5000, 100, false)));
    ASSERT_TRUE(table.Start(Toggle(SpanKey{MODULE_TYPE::MAESTRO_RANDOM, 1, 4}, 1000, 5000, 100, false)));
    ASSERT_TRUE(table.Start(Toggle(SpanKey{MODULE_TYPE::MAESTRO_RANDOM, 1, 5}, 1000, 5000, 100, false)));
    EXPECT_EQ(4u, table.ActiveCount());
}

// A replacement must find its key even when a free slot comes first, or one
// channel would run two spans.
TEST(RandomEffects, TableReplacesSameKeyBehindAFreeSlot)
{
    RandomSpanTable table(2, NoDraws());
    SpanKey shortKey{MODULE_TYPE::GPIO_RANDOM, 0, 1};
    ASSERT_TRUE(table.Start(Toggle(shortKey, 1000, 100, 100, false)));  // slot 0, ends at its first change
    ASSERT_TRUE(table.Start(Toggle(GPIO_KEY, 1000, 5000, 100, false))); // slot 1
    ASSERT_EQ(2u, CollectAt(table, 1000).size());
    ASSERT_EQ(1u, table.ActiveCount()); // slot 0 is free again

    ASSERT_TRUE(table.Start(Toggle(GPIO_KEY, 1050, 5000, 100, true)));
    EXPECT_EQ(1u, table.ActiveCount());
    EXPECT_EQ(1u, CollectAt(table, 1100).size());
}

TEST(RandomEffects, TableStartRejectsBadGaps)
{
    RandomSpanTable table(4, NoDraws());
    ASSERT_TRUE(table.Start(Toggle(GPIO_KEY, 1000, 5000, 100, false)));

    SpanStart zero = Toggle(GPIO_KEY, 1000, 5000, 0, true);
    SpanStart nineteen = Toggle(GPIO_KEY, 1000, 5000, 19, true);
    SpanStart maxBelowMin = Toggle(GPIO_KEY, 1000, 5000, 100, true);
    maxBelowMin.maxGapMs = 99;
    SpanStart spanBelowMin = Toggle(GPIO_KEY, 1000, 99, 100, true);

    EXPECT_FALSE(table.Start(zero));
    EXPECT_FALSE(table.Start(nineteen));
    EXPECT_FALSE(table.Start(maxBelowMin));
    EXPECT_FALSE(table.Start(spanBelowMin));
    EXPECT_EQ(1u, table.ActiveCount());

    // The original span is untouched: it still toggles from low.
    auto changes = CollectAt(table, 1000);
    ASSERT_EQ(1u, changes.size());
    EXPECT_TRUE(changes[0].level);

    // Boundaries are accepted: minGap 20, maxGap == minGap, span == minGap.
    RandomSpanTable fresh(4, NoDraws());
    EXPECT_TRUE(fresh.Start(Toggle(GPIO_KEY, 1000, 20, 20, false)));
}

TEST(RandomEffects, TableFullRejectsAndKeepsExisting)
{
    RandomSpanTable table(2, NoDraws());
    SpanKey a{MODULE_TYPE::GPIO_RANDOM, 0, 1};
    SpanKey b{MODULE_TYPE::GPIO_RANDOM, 0, 2};
    SpanKey c{MODULE_TYPE::GPIO_RANDOM, 0, 3};
    ASSERT_TRUE(table.Start(Toggle(a, 1000, 5000, 100, false)));
    ASSERT_TRUE(table.Start(Toggle(b, 1000, 5000, 100, false)));

    EXPECT_FALSE(table.Start(Toggle(c, 1000, 5000, 100, false)));
    EXPECT_EQ(2u, table.ActiveCount());

    auto changes = CollectAt(table, 1000);
    ASSERT_EQ(2u, changes.size());
    for (const SpanChange &ch : changes)
    {
        EXPECT_TRUE(SameKey(a, ch.key) || SameKey(b, ch.key));
    }

    // Replacing a key needs no free slot.
    EXPECT_TRUE(table.Start(Toggle(a, 2000, 5000, 100, false)));
    EXPECT_EQ(2u, table.ActiveCount());

    // A span that ends frees its slot.
    RandomSpanTable small(1, NoDraws());
    ASSERT_TRUE(small.Start(Toggle(a, 1000, 100, 100, false)));
    EXPECT_FALSE(small.Start(Toggle(b, 1000, 5000, 100, false)));
    ASSERT_EQ(1u, CollectAt(small, 1000).size()); // next lands on the deadline: dropped
    EXPECT_EQ(0u, small.ActiveCount());
    EXPECT_TRUE(small.Start(Toggle(b, 1000, 5000, 100, false)));
}

TEST(RandomEffects, TableNoChangeInsideEndGuard)
{
    // Deadline 2000. A late pass at deadline - 21 still emits...
    RandomSpanTable early(4, NoDraws());
    ASSERT_TRUE(early.Start(Toggle(GPIO_KEY, 1000, 1000, 100, false)));
    ASSERT_EQ(1u, CollectAt(early, 1000).size());
    EXPECT_EQ(1u, CollectAt(early, 1979).size());
    EXPECT_EQ(0u, early.ActiveCount()); // next is 1979 + 100, past the deadline

    // ...one at deadline - 20 drops the span with no change.
    RandomSpanTable atGuard(4, NoDraws());
    ASSERT_TRUE(atGuard.Start(Toggle(GPIO_KEY, 1000, 1000, 100, false)));
    ASSERT_EQ(1u, CollectAt(atGuard, 1000).size());
    EXPECT_TRUE(CollectAt(atGuard, 1980).empty());
    EXPECT_EQ(0u, atGuard.ActiveCount());

    // So does one past the deadline.
    RandomSpanTable late(4, NoDraws());
    ASSERT_TRUE(late.Start(Toggle(GPIO_KEY, 1000, 1000, 100, false)));
    ASSERT_EQ(1u, CollectAt(late, 1000).size());
    EXPECT_TRUE(CollectAt(late, 2500).empty());
    EXPECT_EQ(0u, late.ActiveCount());

    // A next change landing at deadline - 21 is kept; at deadline - 20 the span
    // is dropped right after emitting.
    RandomSpanTable keep(4, NoDraws());
    ASSERT_TRUE(keep.Start(Toggle(GPIO_KEY, 1000, 1000, 979, false)));
    ASSERT_EQ(1u, CollectAt(keep, 1000).size());
    EXPECT_EQ(1u, keep.ActiveCount());
    EXPECT_EQ(1u, CollectAt(keep, 1979).size());

    RandomSpanTable drop(4, NoDraws());
    ASSERT_TRUE(drop.Start(Toggle(GPIO_KEY, 1000, 1000, 980, false)));
    ASSERT_EQ(1u, CollectAt(drop, 1000).size());
    EXPECT_EQ(0u, drop.ActiveCount());

    // A span no longer than the guard never changes anything.
    RandomSpanTable tiny(4, NoDraws());
    ASSERT_TRUE(tiny.Start(Toggle(GPIO_KEY, 1000, 20, 20, false)));
    EXPECT_TRUE(CollectAt(tiny, 1000).empty());
    EXPECT_EQ(0u, tiny.ActiveCount());
}

TEST(RandomEffects, TableServoGapStretchedByTravel)
{
    // Range 10-90, move 40-none, speed 1, accel 0, 2000 us channel. From 10 the
    // move is forced up; size draw 40 -> 80 % -> 1600 us -> 64000 ms of travel.
    std::deque<uint32_t> draws{40, 40};
    RandomSpanTable table(4, Scripted(draws));
    SpanStart s = Servo(MAESTRO_KEY, 1000, 200000, 100, 10, 90, 40, 0, 10);
    s.speed = 1;
    ASSERT_TRUE(table.Start(s));

    auto first = CollectAt(table, 1000);
    ASSERT_EQ(1u, first.size());
    ASSERT_EQ(90, first[0].targetPct);
    EXPECT_EQ(static_cast<uint32_t>(MoveTravelMs(1600, 1, 0)), table.MsUntilNext(1000));
    EXPECT_TRUE(CollectAt(table, 1000 + 64000 - 1).empty());

    // The way back down is stretched the same (|delta|, not delta).
    auto second = CollectAt(table, 1000 + 64000);
    ASSERT_EQ(1u, second.size());
    ASSERT_EQ(10, second[0].targetPct);
    EXPECT_EQ(64000u, table.MsUntilNext(65000));
}

TEST(RandomEffects, TableServoGapKeepsDrawWhenTravelIsShorter)
{
    // Speed 0 (unlimited): 80 % of 2000 us travels in ceil(40 * 1600 / 255) = 251 ms,
    // under the 1000 ms gap.
    std::deque<uint32_t> draws{40};
    RandomSpanTable table(4, Scripted(draws));
    ASSERT_TRUE(table.Start(Servo(MAESTRO_KEY, 1000, 200000, 1000, 10, 90, 40, 0, 10)));

    ASSERT_EQ(1u, CollectAt(table, 1000).size());
    EXPECT_EQ(1000u, table.MsUntilNext(1000));
}

// #1 of the T-008 review: every other table test uses a fixed gap.
TEST(RandomEffects, TableToggleDrawsEachGap)
{
    std::deque<uint32_t> draws{150, 300, 0};
    RandomSpanTable table(4, Scripted(draws));
    SpanStart s = Toggle(GPIO_KEY, 1000, 5000, 100, false);
    s.maxGapMs = 400;
    ASSERT_TRUE(table.Start(s));

    ASSERT_EQ(1u, CollectAt(table, 1000).size());
    EXPECT_EQ(250u, table.MsUntilNext(1000));
    ASSERT_EQ(1u, CollectAt(table, 1250).size());
    EXPECT_EQ(400u, table.MsUntilNext(1250));
    ASSERT_EQ(1u, CollectAt(table, 1650).size());
    EXPECT_EQ(100u, table.MsUntilNext(1650));
    EXPECT_TRUE(draws.empty());
}

// Pins the ruling on draw order: direction (middle band), size, then gap.
TEST(RandomEffects, TableServoDrawsDirectionSizeThenGap)
{
    std::deque<uint32_t> probe{0, 0};
    Rand32 probeRand = Scripted(probe);
    uint32_t up = NextServoTarget(50, 0, 100, 10, 0, probeRand) > 50 ? 0 : 1;

    // Up by 10 + 5; gap 200 + 300. Travel (300 us at speed 0) is 48 ms, so the gap wins.
    std::deque<uint32_t> draws{up, 5, 300};
    RandomSpanTable table(4, Scripted(draws));
    SpanStart s = Servo(MAESTRO_KEY, 1000, 200000, 200, 0, 100, 10, 0, 50);
    s.maxGapMs = 800;
    ASSERT_TRUE(table.Start(s));

    auto changes = CollectAt(table, 1000);
    ASSERT_EQ(1u, changes.size());
    EXPECT_EQ(65, changes[0].targetPct);
    EXPECT_EQ(500u, table.MsUntilNext(1000));
    EXPECT_TRUE(draws.empty());
}

// #2 of the T-008 review: the next change is scheduled from when Collect ran,
// not from when the change was due, so a late pass never causes a catch-up burst.
TEST(RandomEffects, TableSchedulesFromCollectTime)
{
    RandomSpanTable table(4, NoDraws());
    ASSERT_TRUE(table.Start(Toggle(GPIO_KEY, 1000, 5000, 100, false)));

    ASSERT_EQ(1u, CollectAt(table, 1050).size()); // due at 1000, collected late
    EXPECT_EQ(100u, table.MsUntilNext(1050));
    EXPECT_TRUE(CollectAt(table, 1149).empty());
    EXPECT_EQ(1u, CollectAt(table, 1150).size());
}

TEST(RandomEffects, TableReplacementStartsFromItsInitialPct)
{
    std::deque<uint32_t> draws{0, 0};
    RandomSpanTable table(4, Scripted(draws));
    ASSERT_TRUE(table.Start(Servo(MAESTRO_KEY, 1000, 5000, 400, 20, 100, 15, 60, 20)));
    auto first = CollectAt(table, 1000);
    ASSERT_EQ(1u, first.size());
    ASSERT_EQ(35, first[0].targetPct);

    // The replacement starts at 100 (forced down), not at the old span's 35 (forced up).
    ASSERT_TRUE(table.Start(Servo(MAESTRO_KEY, 1100, 5000, 400, 20, 100, 15, 60, 100)));
    auto second = CollectAt(table, 1100);
    ASSERT_EQ(1u, second.size());
    EXPECT_EQ(85, second[0].targetPct);
}

// |delta pct| * rangeUs is computed in 64 bits: 80 * 30,000,000 overflows int.
TEST(RandomEffects, TableServoTravelProductIs64Bit)
{
    std::deque<uint32_t> draws{40};
    RandomSpanTable table(4, Scripted(draws));
    SpanStart s = Servo(MAESTRO_KEY, 1000, INT_MAX, 100, 10, 90, 40, 0, 10);
    s.speed = 1;
    s.rangeUs = 30000000;
    ASSERT_TRUE(table.Start(s));

    ASSERT_EQ(1u, CollectAt(table, 1000).size());
    EXPECT_EQ(static_cast<uint32_t>(MoveTravelMs(24000000, 1, 0)), table.MsUntilNext(1000));
}

TEST(RandomEffects, TableMsUntilNext)
{
    RandomSpanTable table(4, NoDraws());
    EXPECT_EQ(UINT32_MAX, table.MsUntilNext(0));

    ASSERT_TRUE(table.Start(Toggle(GPIO_KEY, 1000, 5000, 100, false)));
    ASSERT_TRUE(table.Start(Toggle(MAESTRO_KEY, 1300, 5000, 100, false)));
    EXPECT_EQ(100u, table.MsUntilNext(900)); // the earlier of the two
    EXPECT_EQ(0u, table.MsUntilNext(1000));  // due
    EXPECT_EQ(0u, table.MsUntilNext(1500));  // overdue

    ASSERT_EQ(1u, CollectAt(table, 1000).size());
    EXPECT_EQ(50u, table.MsUntilNext(1050)); // GPIO next at 1100

    table.Clear();
    EXPECT_EQ(UINT32_MAX, table.MsUntilNext(1050));
}

TEST(RandomEffects, TableClearEmpties)
{
    RandomSpanTable table(3, NoDraws());
    ASSERT_TRUE(table.Start(Toggle(SpanKey{MODULE_TYPE::GPIO_RANDOM, 0, 1}, 1000, 5000, 100, false)));
    ASSERT_TRUE(table.Start(Toggle(SpanKey{MODULE_TYPE::GPIO_RANDOM, 0, 2}, 1000, 5000, 100, false)));
    ASSERT_TRUE(table.Start(Toggle(SpanKey{MODULE_TYPE::GPIO_RANDOM, 0, 3}, 1000, 5000, 100, false)));

    table.Clear();
    EXPECT_EQ(0u, table.ActiveCount());
    EXPECT_TRUE(CollectAt(table, 1000).empty());

    // Every slot is free again.
    ASSERT_TRUE(table.Start(Toggle(SpanKey{MODULE_TYPE::GPIO_RANDOM, 0, 4}, 1000, 5000, 100, false)));
    ASSERT_TRUE(table.Start(Toggle(SpanKey{MODULE_TYPE::GPIO_RANDOM, 0, 5}, 1000, 5000, 100, false)));
    ASSERT_TRUE(table.Start(Toggle(SpanKey{MODULE_TYPE::GPIO_RANDOM, 0, 6}, 1000, 5000, 100, false)));
    EXPECT_EQ(3u, table.ActiveCount());
}

TEST(RandomEffects, TableWrapsAround)
{
    // Starts 50 ms before the uint32 wrap; the deadline lands at 949 after it.
    const uint32_t start = UINT32_MAX - 50;
    RandomSpanTable table(4, NoDraws());
    ASSERT_TRUE(table.Start(Toggle(GPIO_KEY, start, 1000, 100, false)));

    EXPECT_TRUE(CollectAt(table, start - 1).empty());
    ASSERT_EQ(1u, CollectAt(table, start).size());

    // next (start + 100 = 49) is past the wrap; now (start + 10) is not.
    EXPECT_TRUE(CollectAt(table, start + 10).empty());
    EXPECT_EQ(90u, table.MsUntilNext(start + 10));

    int emitted = 1;
    for (uint32_t i = 1; i < 10; i++)
    {
        uint32_t now = start + i * 100; // wrapped from here on
        EXPECT_TRUE(CollectAt(table, now - 1).empty()) << "step " << i;
        emitted += static_cast<int>(CollectAt(table, now).size());
    }
    EXPECT_EQ(10, emitted); // start + 0, 100, ... 900; next would be the deadline
    EXPECT_EQ(0u, table.ActiveCount());
}

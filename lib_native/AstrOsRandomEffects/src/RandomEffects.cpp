#include "RandomEffects.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>

namespace
{
    // § C all-spans rule; T-007's parser checks the same (RandomCommands.cpp).
    constexpr int MIN_GAP_MS = 20;

    int64_t CeilDiv64(int64_t a, int64_t b)
    {
        return (a + b - 1) / b;
    }

    // Smallest n >= 0 with n * n >= x.
    int64_t CeilSqrt64(int64_t x)
    {
        int64_t n = static_cast<int64_t>(std::sqrt(static_cast<double>(x)));
        while (n > 0 && (n - 1) * (n - 1) >= x)
        {
            --n;
        }
        while (n * n < x)
        {
            ++n;
        }
        return n;
    }

    bool SameKey(const SpanKey &a, const SpanKey &b)
    {
        return a.type == b.type && a.module == b.module && a.channel == b.channel;
    }

    // Wrap-safe uint32 ms comparisons, valid while the times are < 2^31 ms apart.
    bool AtOrAfter(uint32_t a, uint32_t b)
    {
        return static_cast<int32_t>(a - b) >= 0;
    }

    // True when t is inside the end guard or past the deadline.
    bool InEndGuard(uint32_t deadlineMs, uint32_t t)
    {
        return static_cast<int32_t>(deadlineMs - t) <= RandomSpanTable::END_GUARD_MS;
    }
} // namespace

int UniformInt(int lo, int hi, const Rand32 &rand)
{
    if (hi <= lo)
    {
        return lo;
    }

    // Up to 2^32 values (INT_MIN..INT_MAX), so count in 64 bits.
    uint64_t count = static_cast<uint64_t>(static_cast<int64_t>(hi) - lo) + 1;
    return static_cast<int>(lo + static_cast<int64_t>(rand() % count));
}

int NextGapMs(int minGapMs, int maxGapMs, const Rand32 &rand)
{
    return UniformInt(minGapMs, maxGapMs, rand);
}

int NextServoTarget(int currentPct, int minPos, int maxPos, int minMove, int maxMove, const Rand32 &rand)
{
    if (currentPct < minPos || currentPct > maxPos)
    {
        return UniformInt(minPos, maxPos, rand);
    }

    bool up;
    if (currentPct <= minPos + minMove)
    {
        up = true;
    }
    else if (currentPct >= maxPos - minMove)
    {
        up = false;
    }
    else
    {
        up = (rand() & 1) != 0;
    }

    int room = up ? maxPos - currentPct : currentPct - minPos;
    int largest = maxMove == 0 ? room : std::min(maxMove, room);
    int size = UniformInt(minMove, largest, rand);

    return std::clamp(up ? currentPct + size : currentPct - size, minPos, maxPos);
}

// WorstCaseTravelMs's model (AstrOsServoUtils.hpp) with the move's distance d in
// place of the 3000 us guard range. Speed unit 0.25 us / 10 ms, so cruising d
// takes 40 d / cap ms; ramping to the cap takes 80 cap / accel ms and covers
// cap^2 / accel us, so the cap is reached when 2 cap^2 <= d accel (trapezoid).
// Each ramp covers its distance at half the cap, so the two ramps add
// 80 cap / accel ms over cruising all of d.
// Otherwise the move is a triangle: sqrt(12800 d / accel) ms.
int MoveTravelMs(int distanceUs, int speed, int acceleration)
{
    if (distanceUs <= 0)
    {
        return 0;
    }

    int64_t distance = distanceUs;
    int64_t accel = std::clamp(acceleration, 0, 255);
    int64_t cap = std::clamp(speed, 0, 255);
    if (cap == 0)
    {
        cap = 255;
    }

    int64_t ms;
    if (accel == 0)
    {
        ms = CeilDiv64(40 * distance, cap);
    }
    else if (2 * cap * cap <= distance * accel)
    {
        ms = CeilDiv64(40 * distance, cap) + CeilDiv64(80 * cap, accel);
    }
    else
    {
        ms = CeilSqrt64(CeilDiv64(12800 * distance, accel));
    }

    return static_cast<int>(std::min<int64_t>(ms, INT_MAX));
}

RandomSpanTable::RandomSpanTable(size_t capacity, Rand32 rng) : slots_(capacity), rng_(std::move(rng)) {}

bool RandomSpanTable::Start(const SpanStart &start)
{
    // Defense in depth: a zero gap would make Collect emit on every pass.
    if (start.minGapMs < MIN_GAP_MS || start.maxGapMs < start.minGapMs || start.spanMs < start.minGapMs)
    {
        return false;
    }

    // An active span with the same key wins over the first free slot.
    Entry *slot = nullptr;
    for (Entry &e : slots_)
    {
        if (e.active && SameKey(e.span.key, start.key))
        {
            slot = &e;
            break;
        }
        if (!e.active && slot == nullptr)
        {
            slot = &e;
        }
    }

    if (slot == nullptr)
    {
        return false;
    }

    *slot = Entry{true,
                  start,
                  start.startMs,
                  start.startMs + static_cast<uint32_t>(start.spanMs),
                  start.initialPct,
                  start.initialLevel};
    return true;
}

void RandomSpanTable::Clear()
{
    for (Entry &e : slots_)
    {
        e.active = false;
    }
}

void RandomSpanTable::Collect(uint32_t nowMs, std::vector<SpanChange> &out)
{
    for (Entry &e : slots_)
    {
        if (!e.active || !AtOrAfter(nowMs, e.nextMs))
        {
            continue;
        }

        if (InEndGuard(e.deadlineMs, nowMs))
        {
            e.active = false;
            continue;
        }

        const SpanStart &span = e.span;
        SpanChange change{span.key, span.isServo, 0, false, 0, 0, e.deadlineMs};
        int gapMs;

        if (span.isServo)
        {
            int target = NextServoTarget(e.currentPct, span.minPos, span.maxPos, span.minMove, span.maxMove, rng_);
            // + 1: GetRelativeRequestedPosition truncates each endpoint in double
            // precision, so the mapped distance can exceed the floored product by 1 us.
            int64_t product = static_cast<int64_t>(std::abs(target - e.currentPct)) * span.rangeUs;
            int64_t distanceUs = product > 0 ? product / 100 + 1 : 0;
            int travelMs =
                MoveTravelMs(static_cast<int>(std::min<int64_t>(distanceUs, INT_MAX)), span.speed, span.acceleration);
            gapMs = std::max(NextGapMs(span.minGapMs, span.maxGapMs, rng_), travelMs);

            e.currentPct = target;
            change.targetPct = target;
            change.speed = span.speed;
            change.acceleration = span.acceleration;
        }
        else
        {
            e.level = !e.level;
            change.level = e.level;
            gapMs = NextGapMs(span.minGapMs, span.maxGapMs, rng_);
        }

        out.push_back(change);

        e.nextMs = nowMs + static_cast<uint32_t>(gapMs);
        if (InEndGuard(e.deadlineMs, e.nextMs))
        {
            e.active = false;
        }
    }
}

uint32_t RandomSpanTable::MsUntilNext(uint32_t nowMs) const
{
    uint32_t soonest = UINT32_MAX;
    for (const Entry &e : slots_)
    {
        if (!e.active)
        {
            continue;
        }

        int32_t until = static_cast<int32_t>(e.nextMs - nowMs);
        soonest = std::min(soonest, until <= 0 ? 0u : static_cast<uint32_t>(until));
    }
    return soonest;
}

size_t RandomSpanTable::ActiveCount() const
{
    return std::count_if(slots_.begin(), slots_.end(), [](const Entry &e) { return e.active; });
}

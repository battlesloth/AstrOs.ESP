#include "RandomSpanAdmission.hpp"

#include <AstrOsEnums.h>
#include <AstrOsServoUtils.hpp>

#include <cstdio>

namespace
{
    // A digital Maestro channel is high at 2500 us and low at 500 us; QueueCommand
    // maps any target >= 1500 to high.
    constexpr int DIGITAL_HIGH_US = 2500;
    constexpr int DIGITAL_LOW_US = 500;
    constexpr int DIGITAL_THRESHOLD_US = 1500;

    bool SameKey(const SpanKey &a, const SpanKey &b)
    {
        return a.type == b.type && a.module == b.module && a.channel == b.channel;
    }

    SpanBuildResult Rejected(SpanReject reason)
    {
        SpanBuildResult r{};
        r.reject = reason;
        return r;
    }

    // Value-initialized, so the fields of the other kind stay zero (Start copies them all).
    SpanStart CommonStart(SpanKey key, uint32_t startMs, int spanMs, int minGapMs, int maxGapMs)
    {
        SpanStart s{};
        s.key = key;
        s.startMs = startMs;
        s.spanMs = spanMs;
        s.minGapMs = minGapMs;
        s.maxGapMs = maxGapMs;
        return s;
    }
} // namespace

SpanBuildResult BuildMaestroSpan(const RandomMaestroCommand &cmd, const MaestroChannelSnapshot &ch, uint32_t startMs)
{
    if (!ch.enabled)
    {
        return Rejected(SpanReject::DISABLED_CHANNEL);
    }

    SpanBuildResult r{};
    r.reject = SpanReject::NONE;

    if (ch.isServo)
    {
        if (ch.maxPos <= ch.minPos)
        {
            return Rejected(SpanReject::NO_USABLE_RANGE);
        }
        if (cmd.minPos == 0 && cmd.maxPos == 0 && cmd.minMove == 0 && cmd.maxMove == 0)
        {
            return Rejected(SpanReject::DIGITAL_SPAN_ON_SERVO);
        }
        if (!RandomServoFieldsValid(cmd))
        {
            return Rejected(SpanReject::INVALID_SERVO_FIELDS);
        }
    }
    else
    {
        r.servoFieldsOnDigital = cmd.minPos != 0 || cmd.maxPos != 0 || cmd.minMove != 0 || cmd.maxMove != 0 ||
                                 cmd.speed != 0 || cmd.acceleration != 0;
    }

    if (cmd.spanMs <= RandomSpanTable::END_GUARD_MS)
    {
        return Rejected(SpanReject::WITHIN_END_GUARD);
    }

    r.start = CommonStart(SpanKey{MODULE_TYPE::MAESTRO_RANDOM, cmd.module, cmd.channel}, startMs, cmd.spanMs,
                          cmd.minGapMs, cmd.maxGapMs);
    if (ch.isServo)
    {
        r.start.isServo = true;
        r.start.minPos = cmd.minPos;
        r.start.maxPos = cmd.maxPos;
        r.start.minMove = cmd.minMove;
        r.start.maxMove = cmd.maxMove;
        r.start.speed = cmd.speed;
        r.start.acceleration = cmd.acceleration;
        r.start.rangeUs = ch.maxPos - ch.minPos;
        r.start.initialPct = PercentFromPosition(ch.minPos, ch.maxPos, ch.lastPos, ch.inverted);
    }
    else
    {
        r.start.initialLevel = ch.lastPos >= DIGITAL_THRESHOLD_US;
    }
    return r;
}

SpanBuildResult BuildGpioSpan(const RandomGpioCommand &cmd, bool level, uint32_t startMs)
{
    if (cmd.spanMs <= RandomSpanTable::END_GUARD_MS)
    {
        return Rejected(SpanReject::WITHIN_END_GUARD);
    }

    SpanBuildResult r{};
    r.reject = SpanReject::NONE;
    r.start =
        CommonStart(SpanKey{MODULE_TYPE::GPIO_RANDOM, 0, cmd.channel}, startMs, cmd.spanMs, cmd.minGapMs, cmd.maxGapMs);
    r.start.initialLevel = level;
    return r;
}

bool SpanArrivedTooLate(uint32_t dispatchMs, int spanMs, uint32_t nowMs)
{
    uint32_t lastChanceMs = dispatchMs + static_cast<uint32_t>(spanMs - RandomSpanTable::END_GUARD_MS);
    return static_cast<int32_t>(nowMs - lastChanceMs) >= 0;
}

void ClearFence::Mark(uint32_t nowMs)
{
    marked_ = true;
    clearMs_ = nowMs;
}

void ClearFence::Expire(uint32_t nowMs)
{
    if (marked_ && nowMs - clearMs_ >= FORGET_AFTER_MS)
    {
        marked_ = false;
    }
}

bool ClearFence::Fences(uint32_t dispatchMs) const
{
    return marked_ && static_cast<int32_t>(dispatchMs - clearMs_) <= 0;
}

size_t FormatSpanChange(const SpanChange &change, char *buf, size_t size)
{
    int written;
    if (change.key.type == MODULE_TYPE::MAESTRO_RANDOM && change.isServo)
    {
        written = snprintf(buf, size, "1|0|%d|%d|%d|%d|%d", change.key.module, change.key.channel, change.targetPct,
                           change.speed, change.acceleration);
    }
    else if (change.key.type == MODULE_TYPE::MAESTRO_RANDOM)
    {
        written = snprintf(buf, size, "1|0|%d|%d|%d|0|0", change.key.module, change.key.channel,
                           change.level ? DIGITAL_HIGH_US : DIGITAL_LOW_US);
    }
    else if (change.key.type == MODULE_TYPE::GPIO_RANDOM)
    {
        written = snprintf(buf, size, "5|0|%d|%d", change.key.channel, change.level ? 1 : 0);
    }
    else
    {
        return 0;
    }

    if (written < 0 || static_cast<size_t>(written) >= size)
    {
        return 0;
    }
    return static_cast<size_t>(written);
}

DropLogThrottle::DropLogThrottle(size_t capacity) : keys_(capacity), used_(capacity, false) {}

void DropLogThrottle::Reset(const SpanKey &key)
{
    for (size_t i = 0; i < keys_.size(); i++)
    {
        if (used_[i] && SameKey(keys_[i], key))
        {
            used_[i] = false;
        }
    }
}

bool DropLogThrottle::ShouldLog(const SpanKey &key)
{
    size_t freeSlot = keys_.size();
    for (size_t i = 0; i < keys_.size(); i++)
    {
        if (used_[i] && SameKey(keys_[i], key))
        {
            return false;
        }
        if (!used_[i] && freeSlot == keys_.size())
        {
            freeSlot = i;
        }
    }

    if (keys_.empty())
    {
        return true;
    }

    size_t slot = freeSlot;
    if (slot == keys_.size())
    {
        slot = next_;
        next_ = (next_ + 1) % keys_.size();
    }
    keys_[slot] = key;
    used_[slot] = true;
    return true;
}

uint32_t WaitTicksCeil(uint32_t ms, uint32_t tickPeriodMs)
{
    return ms / tickPeriodMs + (ms % tickPeriodMs != 0 ? 1 : 0);
}

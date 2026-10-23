#ifndef RANDOMEFFECTS_HPP
#define RANDOMEFFECTS_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

// Every random decision for the random span script commands (types 6 and 7):
// the servo move rule, gap draws, the travel-time gap stretch and the table of
// active spans. Behavior: .docs/protocol.md § C. Pure, with an injected RNG so
// native tests are deterministic; the random-effects task in src/main.cpp (T-009)
// owns the table and supplies esp_random.

// Must be callable (never empty) and uniform over all 32 bits: UniformInt takes
// draw % count and the low bit picks a servo direction. esp_random on the
// controller; scripted or std::mt19937 in native tests.
using Rand32 = std::function<uint32_t()>;

// Uniform over lo..hi inclusive (draw % count). Returns lo without drawing when
// hi <= lo.
int UniformInt(int lo, int hi, const Rand32 &rand);

// The next gap between changes: uniform over minGapMs..maxGapMs.
int NextGapMs(int minGapMs, int maxGapMs, const Rand32 &rand);

// The next servo target, in percent (§ C move rule):
// - currentPct outside minPos..maxPos: a uniform position inside it (minMove ignored)
// - currentPct <= minPos + minMove: up; else >= maxPos - minMove: down; else the
//   draw's parity picks
// - size uniform over minMove..min(maxMove, room in that direction); maxMove 0 = no max
// - the result is clamped to minPos..maxPos (reachable only when minMove > range / 2)
// Inputs are § C percents (T-009 checks RandomServoFieldsValid before Start), so
// the arithmetic isn't overflow-checked.
int NextServoTarget(int currentPct, int minPos, int maxPos, int minMove, int maxMove, const Rand32 &rand);

// Travel time for a move of distanceUs: the Maestro speed/acceleration model of
// WorstCaseTravelMs evaluated at distanceUs (so it equals WorstCaseTravelMs at
// 3000 us). Not a linear scale: a move that never reaches the speed cap takes
// sqrt-of-distance time. Speed 0 = 255; speed and acceleration clamped to 0-255;
// every division ceils. 0 when distanceUs <= 0; saturates at INT_MAX.
int MoveTravelMs(int distanceUs, int speed, int acceleration);

// type is a MODULE_TYPE value (MAESTRO_RANDOM or GPIO_RANDOM); module is the
// Maestro index (0 for GPIO). Two spans with an equal key are the same span.
struct SpanKey
{
    int type;
    int module;
    int channel;
};

// Value-initialize it (SpanStart s{} or designated initializers) so the fields
// of the other kind are zero: Start copies all of them. Start checks only the
// all-span gap rules. Everything else is trusted to the caller (T-009):
// - servo fields are § C-valid (RandomServoFieldsValid, which also requires
//   minGapMs >= 100 for a servo)
// - isServo only with MAESTRO_RANDOM
// - rangeUs > 0 (<= 0 disables the travel stretch)
// - initialPct 0-100
struct SpanStart
{
    SpanKey key;
    uint32_t startMs; // first change is due here; the span ends at startMs + spanMs
    int spanMs;
    int minGapMs;
    int maxGapMs;
    bool isServo; // false = toggle (GPIO or digital Maestro)
    // servo only
    int minPos;
    int maxPos;
    int minMove;
    int maxMove; // 0 = no max
    int speed;
    int acceleration;
    int rangeUs;    // channel maxPos - minPos in us, for the travel stretch
    int initialPct; // script percent at start, before MaestroModule's inversion (PercentFromPosition)
    // toggle only
    bool initialLevel;
};

// One due change; the fields of the other kind are 0 / false. Dispatch needs
// both isServo and key.type:
// - isServo: a servo move (targetPct, speed, acceleration)
// - !isServo, MAESTRO_RANDOM: a digital Maestro channel; level is 2500 or 500 us
// - GPIO_RANDOM: level is 1 or 0
struct SpanChange
{
    SpanKey key;
    bool isServo;
    int targetPct;
    bool level;
    int speed;
    int acceleration;
};

// Fixed-capacity table of active spans. The table allocates only in its
// constructor; Collect appends to the caller's vector (reserve it to keep the
// loop allocation-free). Not thread-safe: T-009 guards it with randomTableMutex.
// Times are uint32 milliseconds, compared wrap-safe.
class RandomSpanTable
{
public:
    // No change is emitted at or after deadline - END_GUARD_MS.
    static constexpr int END_GUARD_MS = 20;

    RandomSpanTable(size_t capacity, Rand32 rng);

    // A copy would fork the schedule (both copies would emit the same changes).
    RandomSpanTable(const RandomSpanTable &) = delete;
    RandomSpanTable &operator=(const RandomSpanTable &) = delete;

    // Replaces an active span with an equal key, else takes a free slot. False,
    // with the table unchanged, when the table is full or § C's all-span gap
    // rules fail (minGapMs < 20, maxGapMs < minGapMs, spanMs < minGapMs).
    // True means scheduled: a span of END_GUARD_MS or less is accepted but never
    // changes anything.
    [[nodiscard]] bool Start(const SpanStart &start);

    void Clear();

    // Appends one change per due span and schedules its next one; drops spans
    // that are inside the end guard (with no change) or whose next change would be.
    void Collect(uint32_t nowMs, std::vector<SpanChange> &out);

    // Ms until the earliest scheduled change: 0 when one is due, UINT32_MAX when
    // empty. A caller converting to RTOS ticks must round up, or a 1-9 ms wait at
    // 100 Hz becomes 0 ticks and the caller spins until the change is due.
    uint32_t MsUntilNext(uint32_t nowMs) const;

    size_t ActiveCount() const;

private:
    struct Entry
    {
        bool active;
        SpanStart span;
        uint32_t nextMs;
        uint32_t deadlineMs;
        int currentPct; // servo
        bool level;     // toggle
    };

    std::vector<Entry> slots_;
    Rand32 rng_;
};

#endif

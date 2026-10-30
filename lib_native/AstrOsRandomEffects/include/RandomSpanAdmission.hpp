#ifndef RANDOMSPANADMISSION_HPP
#define RANDOMSPANADMISSION_HPP

#include <RandomCommands.hpp>
#include <RandomEffects.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

// The random-effects task's pure decisions around the span table (T-009):
// turning a parsed command plus the channel's state into a SpanStart, the
// timing checks before Start, the type-1 / type-5 strings a change becomes, and
// the log throttle for dropped changes. Behavior: .docs/protocol.md § C. The
// MIXED caller in src/main.cpp takes the locks, reads the channels and logs.

// A Maestro channel as the random task needs it, read under the module's
// stateMutex (MaestroModule::GetChannelSnapshot). Positions are in us.
struct MaestroChannelSnapshot
{
    bool enabled;
    bool isServo;
    int minPos;
    int maxPos;
    int home;
    bool inverted;
    int lastPos; // last commanded target (T-006); a digital channel's is 500 or 2500
};

// Why a span can't start. The caller logs it with the values it names.
enum class SpanReject
{
    NONE,
    DISABLED_CHANNEL,
    NO_USABLE_RANGE,       // servo channel with maxPos <= minPos: nothing would move
    DIGITAL_SPAN_ON_SERVO, // config mismatch: servo channel, but positions and moves all 0
    INVALID_SERVO_FIELDS,  // RandomServoFieldsValid failed
    WITHIN_END_GUARD       // spanMs <= RandomSpanTable::END_GUARD_MS: no change could run
};

struct [[nodiscard]] SpanBuildResult
{
    SpanReject reject;
    // Digital channel, but a servo field is non-zero: the server thinks it is a
    // servo. The span still runs as a toggle; the caller warns.
    bool servoFieldsOnDigital;
    SpanStart start; // meaningful only when reject == NONE
};

// Checks, in order: disabled; for a servo, no usable range, then a digital span
// on it, then RandomServoFieldsValid; then the end guard. A servo starts from
// PercentFromPosition(lastPos) over the channel's range; a digital channel from
// lastPos >= 1500. The fields of the other kind are left zero.
SpanBuildResult BuildMaestroSpan(const RandomMaestroCommand &cmd, const MaestroChannelSnapshot &ch, uint32_t startMs);

// A GPIO toggle keyed {GPIO_RANDOM, 0, channel}, starting from level.
SpanBuildResult BuildGpioSpan(const RandomGpioCommand &cmd, bool level, uint32_t startMs);

// True when a span dispatched at dispatchMs can no longer make a change: nowMs
// is at or past dispatchMs + spanMs - END_GUARD_MS. Start would accept it and
// Collect would drop it without a word, so the caller rejects it with a log.
// Wrap-safe while the times are < 2^31 ms apart.
bool SpanArrivedTooLate(uint32_t dispatchMs, int spanMs, uint32_t nowMs);

// Panic and config reload clear the table. A span dispatched at or before the
// latest clear must not start after it, even if it was already in flight.
// Not thread-safe: the caller holds randomTableMutex around every call.
class ClearFence
{
public:
    // A message the random task holds was dispatched milliseconds ago, so a
    // clear this old can't fence it. Forgetting it keeps the wrap-safe
    // comparison inside its 2^31 ms window.
    static constexpr uint32_t FORGET_AFTER_MS = 1u << 30; // ~12.4 days

    void Mark(uint32_t nowMs);

    // Call on every pass (at least once per FORGET_AFTER_MS): forgets a clear
    // that is FORGET_AFTER_MS old.
    void Expire(uint32_t nowMs);

    // True when dispatchMs is at or before the remembered clear.
    bool Fences(uint32_t dispatchMs) const;

private:
    bool marked_ = false;
    uint32_t clearMs_ = 0;
};

// Room for the longest change string ("1|0|" and five ints with separators) and its NUL.
constexpr size_t SPAN_CHANGE_MAX_LEN = 64;

// Writes the command a change becomes, with delta 0 (.docs/protocol.md § C):
// - servo:           1|0|module|channel|targetPct|speed|accel
// - digital Maestro: 1|0|module|channel|2500 or 500|0|0
// - GPIO:            5|0|channel|1 or 0
// Returns the length written (NUL-terminated), or 0 when the key's type isn't
// MAESTRO_RANDOM / GPIO_RANDOM or the string doesn't fit in size.
size_t FormatSpanChange(const SpanChange &change, char *buf, size_t size);

// Remembers which spans already logged a dropped change, so a full queue logs
// once per span rather than once per change. Fixed capacity, allocated in the
// constructor; when full, a new key replaces one round-robin (the replaced key
// just logs again).
class DropLogThrottle
{
public:
    explicit DropLogThrottle(size_t capacity);

    // On a successful Start: the span's next drop logs again.
    void Reset(const SpanKey &key);

    // True the first time a key is seen since its Reset; remembers it.
    bool ShouldLog(const SpanKey &key);

private:
    std::vector<SpanKey> keys_;
    std::vector<bool> used_;
    size_t next_ = 0; // slot the next new key takes when none is free
};

// Random changes may fill at most half of a change queue (servoQueue, gpioQueue);
// the other half stays free for the script's own commands. The queue tasks
// drain about one message per 10 ms tick, and the random task outranks the
// dispatch loop, so spans changing faster than that would otherwise keep a queue
// full: the dispatch loop then waits 2 s on its send and drops the scripted
// command. True while waiting < spaces (fewer than half the slots in use).
bool RandomChangeHasRoom(size_t waiting, size_t spaces);

// Ticks for an RTOS wait of ms, rounded up so a 1-9 ms wait at 100 Hz is 1 tick,
// not 0 (a 0-tick wait would spin until the change is due). Never overflows.
uint32_t WaitTicksCeil(uint32_t ms, uint32_t tickPeriodMs);

#endif

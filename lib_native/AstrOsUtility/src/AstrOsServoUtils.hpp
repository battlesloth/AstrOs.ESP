#ifndef ASTROSSERVOUTILS_H
#define ASTROSSERVOUTILS_H

#include <AstrOsStructs.h>
#include <algorithm>
#include <cstdint>
#include <math.h>
#include <string>

/// @brief takes a microsecond value and frequency in micorsecond
// and converts it to a step value for the PCA9685 based on the
// frequency that the board is set to and 4096 division for the
// boards 12-bit register.
/// @param us
/// @param freq_us
/// @return
int GetMicroSecondsAsStep(int us, double freq_us);

/// @brief take a microsecond value and convert it to a position in the step map
// based on the number of steps passed.
/// @param val
/// @param freq_us
/// @param minStep
/// @param maxStep
/// @return
int MicroSecondsToMapPosition(int val, double freq_us, int minStep, int maxStep, int steps);

/// @brief calculate the step map for the PCA9685 based on the frequency.
// The board has 4096 steps, so we precalculate the step value for value
// to move the servo a fraction of a degrees based on the steps passed (360 for 1/2).
// This assumes the servo range is 500 us to 2500 us at a range of 180 degrees.
/// @param freq frequency in Hz
/// @param map the map to fill
/// @return return the step size so we can calculate requested positions
double CalculateStepMap(double freq, uint16_t *map, int steps);

/// @brief Get the postion requested based on percentage difference between
/// the min and max position
/// @param minPos
/// @param maxPos
/// @param requestPercentage
/// @return
int GetRelativeRequestedPosition(int minPos, int maxPos, int requestPercentage);

/// @brief The inverse of GetRelativeRequestedPosition: the script percent a
/// channel position corresponds to, rounded to the nearest percent and clamped
/// to 0-100. When inverted, returns 100 - p, undoing the 100 - p that
/// MaestroModule applies before GetRelativeRequestedPosition. A channel with
/// maxUs <= minUs has no scale and reads as 0 (100 when inverted).
/// @param minUs channel minimum position in microseconds
/// @param maxUs channel maximum position in microseconds
/// @param us position in microseconds
/// @param inverted the channel's inverted flag
/// @return percent, 0-100
int PercentFromPosition(int minUs, int maxUs, int us, bool inverted);

/// @brief Encode a Maestro Set Target payload. The Maestro takes targets in
/// quarter-microseconds as two 7-bit bytes (low bits first); 0 means "stop
/// sending pulses" (servo off) and stays 0. Every SET_TARGET frame goes
/// through this so a µs value encodes the same wherever it is used (T-006:
/// the pre-position frame used to send raw µs, a quarter of the intended
/// pulse).
/// @param us pulse width in microseconds (0 = off)
/// @param lo out: bits 0-6 of the quarter-µs target
/// @param hi out: bits 7-13 of the quarter-µs target
inline void EncodeMaestroTarget(int us, uint8_t &lo, uint8_t &hi)
{
    int quarterUs = us * 4;
    lo = quarterUs & 0x7F;
    hi = (quarterUs >> 7) & 0x7F;
}

// The release floor is per channel (T-010): the channel's releaseMs, sent as
// the 8th field of its config entry (.docs/protocol.md § D). It covers
// full-speed moves and loads whose slew the Maestro speed model does not
// describe (linear actuators run at their own rate regardless of the
// commanded pulse ramp). The default, 5 s, is T-005's global floor (20 s before
// T-005). parseServoConfig accepts only MIN..MAX; anything else falls back to
// the default with a warning, and ServoReleaseDeadlineMs clamps the same way.
constexpr int MAESTRO_RELEASE_DEFAULT_MS = ASTROS_MAESTRO_RELEASE_DEFAULT_MS;
constexpr int MAESTRO_RELEASE_MIN_MS = 1000;
constexpr int MAESTRO_RELEASE_MAX_MS = 120000;
static_assert(MAESTRO_RELEASE_MIN_MS <= MAESTRO_RELEASE_DEFAULT_MS &&
                  MAESTRO_RELEASE_DEFAULT_MS <= MAESTRO_RELEASE_MAX_MS,
              "the default release time must be in the accepted range");
static_assert(servo_channel{}.releaseMs == MAESTRO_RELEASE_DEFAULT_MS,
              "a {}-initialized servo_channel must carry the default release time, never 0 ms");

// Proportional margin on the modelled travel time (T-005). The guard range
// already gives 1.5x on distance; this covers everything else that scales
// with the move (accel quantization, servo lag at low speed).
constexpr int MAESTRO_RELEASE_MARGIN_PERCENT = 10;

/// @brief ceil(a / b) for a >= 0, b > 0
constexpr int CeilDiv(int a, int b)
{
    return (a + b - 1) / b;
}

/// @brief Physical worst-case time for a Maestro servo to cover the full
/// 0-3000 us guard range. The guard range is the margin: a real sweep is at
/// most 2000 us (500-2500), so every result is >= 1.5x the physical time.
/// The remaining error sources (300 ms check tick, serial latency, Maestro
/// 80 ms accel steps) are additive and sub-second; no multiplier is applied.
///
/// Maestro units: speed is 0.25 us per 10 ms per unit, so cruising the guard
/// range takes 120000 / speed ms. Acceleration is 0.25 us per 10 ms per 80 ms
/// per unit, i.e. speed grows by `acceleration` units every 80 ms, so ramping
/// up to the cap takes 80 * speed / acceleration ms. A move is a trapezoid
/// (ramp, cruise, ramp) when the cap is reached before the halfway point,
/// which is speed^2 <= 1500 * acceleration; otherwise it is a triangle and
/// the time is 2 * sqrt(3000 us / a) with a = acceleration * 312.5 us/s^2,
/// i.e. sqrt(38400000 / acceleration) ms.
/// Speed 0 (no limit) is treated as 255; acceleration 0 means no ramp;
/// out-of-range inputs are clamped to 0-255. All divisions ceil.
/// @param speed Maestro speed value (0-255)
/// @param acceleration Maestro acceleration value (0-255)
/// @return worst-case travel in milliseconds
int WorstCaseTravelMs(int speed, int acceleration);

/// @brief The release deadline CheckServos uses: the greater of the
/// channel's floor and the physical worst case (WorstCaseTravelMs) plus
/// MAESTRO_RELEASE_MARGIN_PERCENT, ceiling. The floor is never a cap. A floor
/// outside MAESTRO_RELEASE_MIN_MS..MAX_MS (e.g. 0 from a memset struct) is
/// replaced by MAESTRO_RELEASE_DEFAULT_MS, as the parser does.
/// @param speed Maestro speed value (0-255)
/// @param acceleration Maestro acceleration value (0-255)
/// @param floorMs the channel's release time (servo_channel::releaseMs)
/// @return deadline in milliseconds
int ServoReleaseDeadlineMs(int speed, int acceleration, int floorMs);

/// @brief Of the servos a config reload left energized (`pending`), which still
/// need an off, judged on the live channel state (StepReloadRelease calls it on
/// each CheckServos tick with a pending bit, outside loading). A channel that is tracked `on` again — re-armed by
/// homing or by a new command — needs none: its normal release takes over. Neither does an enabled digital channel:
/// homing set its level, and an off frame would undo it. One the new config disabled, or that homing failed to re-arm,
/// is otherwise never released.
/// @param pending bit i set if channel i was energized when a reload reset its tracking
/// @param channels the live channels
/// @param count number of channels (at most 32)
/// @return bit i set for each channel to send an off to
uint32_t ChannelsToReleaseAfterReload(uint32_t pending, const servo_channel *channels, int count);

/// How an attempt to queue one channel's off went (mirrors MaestroModule's
/// enqueue results, plus a send-mutex try-take that failed).
enum class ReloadOffResult
{
    Queued,
    SendBusy,
    QueueFull,
    NoMemory
};

struct ReloadReleaseOutcome
{
    uint32_t pending;   // bits still waiting for their off
    uint32_t released;  // offs queued this tick
    uint32_t sendBusy;  // retry next tick
    uint32_t queueFull; // retry next tick
    uint32_t noMemory;  // retry next tick
};

/// @brief One CheckServos tick of reload releases (T-010, PR #66 review): drop
/// the bits ChannelsToReleaseAfterReload says need no off, try each remaining
/// off with `trySend(channel)`, and clear a bit only once its off is queued, so
/// a failed enqueue retries next tick instead of leaving the servo powered. Does
/// nothing while `loading` (homing hasn't re-armed its channels yet).
template <typename TrySend>
ReloadReleaseOutcome StepReloadRelease(uint32_t pending, const servo_channel *channels, int count, bool loading,
                                       TrySend trySend)
{
    ReloadReleaseOutcome outcome{pending, 0, 0, 0, 0};
    if (pending == 0 || loading)
    {
        return outcome;
    }
    outcome.pending = ChannelsToReleaseAfterReload(pending, channels, count);
    for (int i = 0; i < count && i < 32; i++)
    {
        uint32_t bit = 1u << i;
        if (!(outcome.pending & bit))
        {
            continue;
        }
        switch (trySend(i))
        {
        case ReloadOffResult::Queued:
            outcome.pending &= ~bit;
            outcome.released |= bit;
            break;
        case ReloadOffResult::SendBusy:
            outcome.sendBusy |= bit;
            break;
        case ReloadOffResult::QueueFull:
            outcome.queueFull |= bit;
            break;
        case ReloadOffResult::NoMemory:
            outcome.noMemory |= bit;
            break;
        }
    }
    return outcome;
}

/// @brief The release times a module applied, for one INFO line after a config
/// load: enabled servo channels whose release time isn't the default, e.g.
/// "ch1 15000 ms; others 5000 ms", or "all 5000 ms (default)".
std::string FormatReleaseTimes(const servo_channel *channels, int count);

#endif

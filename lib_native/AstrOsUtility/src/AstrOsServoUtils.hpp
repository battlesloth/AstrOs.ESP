#ifndef ASTROSSERVOUTILS_H
#define ASTROSSERVOUTILS_H

#include <algorithm>
#include <cstdint>
#include <math.h>

/// @brief takes a microsecond value and frequency in micorsecond
// and converts it to a step value for the PCA9685 based on the
// frequency that the board is set to and 4096 division for the
// boards 12-bit register.
/// @param us
/// @param freq_us
/// @return
int GetMicroSecondsAsStep(int us, double freq_us)
{
    auto asdouble = (double)us;
    auto percent = (asdouble * 100.0) / freq_us; // percent of duty cycle
    auto step = percent / .0244l;                // 4096 steps in the PCA9685
    return (int)std::round(step);
}

/// @brief take a microsecond value and convert it to a position in the step map
// based on the number of steps passed.
/// @param val
/// @param freq_us
/// @param minStep
/// @param maxStep
/// @return
int MicroSecondsToMapPosition(int val, double freq_us, int minStep, int maxStep, int steps)
{
    auto step = GetMicroSecondsAsStep(val, freq_us);

    auto stepSize = (maxStep - minStep) / (double)steps;
    int positionInStepMap = std::round((step - minStep) / stepSize);

    // zero indexed 360 step array
    auto clamped = std::clamp(positionInStepMap, 0, steps - 1);

    return std::round(clamped);
}

/// @brief calculate the step map for the PCA9685 based on the frequency.
// The board has 4096 steps, so we precalculate the step value for value
// to move the servo a fraction of a degrees based on the steps passed (360 for 1/2).
// This assumes the servo range is 500 us to 2500 us at a range of 180 degrees.
/// @param freq frequency in Hz
/// @param map the map to fill
/// @return return the step size so we can calculate requested positions
double CalculateStepMap(double freq, uint16_t *map, int steps)
{
    auto freq_us = 1000000.0 / freq; // frequency in microseconds
    auto minStep = GetMicroSecondsAsStep(500, freq_us);
    auto maxStep = GetMicroSecondsAsStep(2500, freq_us);

    double currentStep = (double)minStep;
    double stepSize = (maxStep - minStep) / (double)steps;

    for (size_t i = 0; i < steps; i++)
    {
        map[i] = std::round(currentStep);
        currentStep += stepSize;
    }

    return stepSize;
}

/// @brief Get the postion requested based on percentage difference between
/// the min and max position
/// @param minPos
/// @param maxPos
/// @param requestPercentage
/// @return
int GetRelativeRequestedPosition(int minPos, int maxPos, int requestPercentage)
{
    if (requestPercentage <= 0)
    {
        return minPos;
    }
    else if (requestPercentage >= 100)
    {
        return maxPos;
    }

    // get the number of steps between the min and max
    int steps = (maxPos - minPos);

    int requestPos = steps * (requestPercentage / 100.0);

    int move = minPos + requestPos;

    return std::clamp(move, minPos, maxPos);
}

// Slack multiplier on the physical worst-case travel time. Covers model
// uncertainty (servo lag behind the commanded pulse, timer jitter). Bench-tune.
constexpr int MAESTRO_RELEASE_SLACK = 4;

// Floor on the release deadline for loads whose slew the Maestro speed model
// does not describe (linear actuators run at their own rate regardless of the
// commanded pulse ramp). 20 s matches the ~19 s full-speed hold the old math
// produced, which those loads were living with. Stand-in until a per-channel
// release setting exists.
constexpr int MAESTRO_RELEASE_FLOOR_MS = 20000;

/// @brief ceil(a / b) for a >= 0, b > 0
constexpr int CeilDiv(int a, int b)
{
    return (a + b - 1) / b;
}

/// @brief smallest n >= 0 with n * n >= x
static int CeilSqrt(int x)
{
    int n = (int)std::sqrt((double)x);
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

/// @brief Physical worst-case time for a Maestro servo to cover the full
/// 0-3000 us guard range, times MAESTRO_RELEASE_SLACK.
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
/// @return worst-case travel in milliseconds, with slack
int WorstCaseTravelMs(int speed, int acceleration)
{
    speed = std::clamp(speed, 0, 255);
    acceleration = std::clamp(acceleration, 0, 255);

    int cap = speed == 0 ? 255 : speed;
    int cruiseMs = CeilDiv(120000, cap);

    int travelMs;
    if (acceleration == 0)
    {
        travelMs = cruiseMs;
    }
    else if (cap * cap <= 1500 * acceleration)
    {
        travelMs = cruiseMs + CeilDiv(80 * cap, acceleration);
    }
    else
    {
        travelMs = CeilSqrt(CeilDiv(38400000, acceleration));
    }

    return travelMs * MAESTRO_RELEASE_SLACK;
}

/// @brief The release deadline CheckServos uses: the physical worst case
/// (WorstCaseTravelMs) floored at MAESTRO_RELEASE_FLOOR_MS.
/// @param speed Maestro speed value (0-255)
/// @param acceleration Maestro acceleration value (0-255)
/// @return deadline in milliseconds
int ServoReleaseDeadlineMs(int speed, int acceleration)
{
    return std::max(WorstCaseTravelMs(speed, acceleration), MAESTRO_RELEASE_FLOOR_MS);
}

#endif

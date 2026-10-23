#include "AstrOsServoUtils.hpp"

// Declarations and doc comments: AstrOsServoUtils.hpp.

int GetMicroSecondsAsStep(int us, double freq_us)
{
    auto asdouble = (double)us;
    auto percent = (asdouble * 100.0) / freq_us; // percent of duty cycle
    auto step = percent / .0244l;                // 4096 steps in the PCA9685
    return (int)std::round(step);
}

int MicroSecondsToMapPosition(int val, double freq_us, int minStep, int maxStep, int steps)
{
    auto step = GetMicroSecondsAsStep(val, freq_us);

    auto stepSize = (maxStep - minStep) / (double)steps;
    int positionInStepMap = std::round((step - minStep) / stepSize);

    // zero indexed 360 step array
    auto clamped = std::clamp(positionInStepMap, 0, steps - 1);

    return std::round(clamped);
}

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

int PercentFromPosition(int minUs, int maxUs, int us, bool inverted)
{
    int percent = 0;
    if (maxUs > minUs)
    {
        // Clamp before rounding: lround returns a 32-bit long on the ESP32.
        double scaled = ((double)us - minUs) * 100.0 / ((double)maxUs - minUs);
        percent = (int)std::lround(std::clamp(scaled, 0.0, 100.0));
    }
    return inverted ? 100 - percent : percent;
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

int WorstCaseTravelMs(int speed, int acceleration)
{
    speed = std::clamp(speed, 0, 255);
    acceleration = std::clamp(acceleration, 0, 255);

    int cap = speed == 0 ? 255 : speed;
    int cruiseMs = CeilDiv(120000, cap);

    if (acceleration == 0)
    {
        return cruiseMs;
    }
    if (cap * cap <= 1500 * acceleration)
    {
        return cruiseMs + CeilDiv(80 * cap, acceleration);
    }
    return CeilSqrt(CeilDiv(38400000, acceleration));
}

int ServoReleaseDeadlineMs(int speed, int acceleration)
{
    int travel = WorstCaseTravelMs(speed, acceleration);
    int withMargin = travel + CeilDiv(travel * MAESTRO_RELEASE_MARGIN_PERCENT, 100);
    return std::max(withMargin, MAESTRO_RELEASE_FLOOR_MS);
}

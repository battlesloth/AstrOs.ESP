#ifndef ASTROSSERVOUTILS_H
#define ASTROSSERVOUTILS_H

#include <math.h>
#include <cstdint>

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
    auto step = percent / .0244l; // 4096 steps in the PCA9685
    return (int) std::round(step);
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

    auto stepSize = (maxStep - minStep) / (double) steps;
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

    double currentStep = (double) minStep;
    double stepSize = (maxStep - minStep) / (double) steps; 

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

#endif
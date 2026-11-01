#pragma once

#include <algorithm>
#include <cmath>

namespace vmp
{

// SL-1200MK2 pitch fader: ±8%, modelled on a 0.05% grid. Pitch is stored as an integer
// number of steps so planned values never drift off the grid through float arithmetic.
inline constexpr double kPitchStepPercent = 0.05;
inline constexpr int kMaxPitchSteps = 160; // 8.00%

inline int clampPitchSteps(int steps)
{
    return std::clamp(steps, -kMaxPitchSteps, kMaxPitchSteps);
}

inline int pitchStepsFromPercent(double percent)
{
    return clampPitchSteps(static_cast<int>(std::lround(percent / kPitchStepPercent)));
}

inline double pitchPercentFromSteps(int steps)
{
    return clampPitchSteps(steps) * kPitchStepPercent;
}

// Playback speed multiplier. On a turntable tempo and pitch move together.
inline double speedFromPitchSteps(int steps)
{
    return 1.0 + pitchPercentFromSteps(steps) / 100.0;
}

// For pitch part way through a slide, which can fall between steps.
inline double speedFromPitchSteps(double steps)
{
    return 1.0 + std::clamp(steps, -double(kMaxPitchSteps), double(kMaxPitchSteps)) * kPitchStepPercent / 100.0;
}

inline double pitchStepsFromSpeed(double speed)
{
    return (speed - 1.0) * 100.0 / kPitchStepPercent;
}

// How far the musical key moves at a given speed: +6% is roughly +1 semitone.
inline double semitoneShiftForSpeed(double speed)
{
    return 12.0 * std::log2(speed);
}

} // namespace vmp

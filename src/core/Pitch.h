#pragma once

#include <algorithm>
#include <cmath>

namespace vmp
{

// SL-1200MK2 pitch fader: ±8%, modelled on a 0.02% grid. The real fader is analog; the grid is
// fine enough that two records matched on it drift apart by only ~6 ms over a 30 s mix, the
// kind of error a DJ rides out by ear. Pitch is stored as a number of steps so planned values
// stay on the grid. Set files record the step size, so it can change (sets made at 0.05% are
// converted on load).
inline constexpr double kPitchStepPercent = 0.02;
inline constexpr int kMaxPitchSteps = 400; // 8.00%

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

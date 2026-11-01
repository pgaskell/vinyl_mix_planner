#include "core/PitchCurve.h"

#include "core/Pitch.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace vmp
{

namespace
{
constexpr double kInfinity = std::numeric_limits<double>::infinity();
// Tolerance for a move starting exactly where the previous one ends, after float round-off.
constexpr double kAnchorEpsilon = 1.0e-9;
} // namespace

PitchCurve::PitchCurve(int baseSteps, std::vector<Move> moves)
{
    std::sort(moves.begin(), moves.end(),
              [](const Move& a, const Move& b) { return a.sourceSeconds < b.sourceSeconds; });

    double t = 0.0, x = 0.0;
    double speed = speedFromPitchSteps(baseSteps);

    for (const auto& move : moves)
    {
        if (!(move.durationSeconds >= 0.0) || !std::isfinite(move.durationSeconds))
            throw std::invalid_argument("pitch move has an invalid duration");
        if (move.sourceSeconds < 0.0)
            continue; // anchored before the needle drop: trimmed away
        if (move.sourceSeconds < x - kAnchorEpsilon)
            throw std::invalid_argument("pitch move starts before the previous one has finished");

        const double hold = (move.sourceSeconds - x) / speed;
        if (hold > 0.0)
        {
            segments_.push_back({t, hold, speed, speed, x});
            t += hold;
            x = move.sourceSeconds;
        }

        const double target = speedFromPitchSteps(move.targetSteps);
        if (move.durationSeconds > 0.0)
        {
            segments_.push_back({t, move.durationSeconds, speed, target, x});
            x += move.durationSeconds * (speed + target) / 2.0;
            t += move.durationSeconds;
        }
        speed = target;
    }

    segments_.push_back({t, kInfinity, speed, speed, x});
}

const PitchCurve::Segment& PitchCurve::segmentAtTime(double t) const
{
    auto it = std::upper_bound(segments_.begin(), segments_.end(), t,
                               [](double value, const Segment& s) { return value < s.t0; });
    return it == segments_.begin() ? segments_.front() : *std::prev(it);
}

const PitchCurve::Segment& PitchCurve::segmentAtSource(double x) const
{
    auto it = std::upper_bound(segments_.begin(), segments_.end(), x,
                               [](double value, const Segment& s) { return value < s.x0; });
    return it == segments_.begin() ? segments_.front() : *std::prev(it);
}

double PitchCurve::speedAt(double t) const
{
    if (t < 0.0)
        return segments_.front().s0;
    const auto& seg = segmentAtTime(t);
    return seg.s0 + seg.acceleration() * (t - seg.t0);
}

double PitchCurve::sourceSecondsAt(double t) const
{
    // Before the needle drop, extrapolate at the starting speed (useful for lead-in drawing).
    if (t < 0.0)
        return segments_.front().s0 * t;
    const auto& seg = segmentAtTime(t);
    const double u = t - seg.t0;
    return seg.x0 + seg.s0 * u + 0.5 * seg.acceleration() * u * u;
}

double PitchCurve::timeAtSourceSeconds(double x) const
{
    if (x < 0.0)
        return x / segments_.front().s0;
    const auto& seg = segmentAtSource(x);
    const double y = x - seg.x0;
    // Root of 0.5*a*u^2 + s0*u - y = 0, in the form that stays stable as a -> 0.
    // Speed never goes below 0.92, so the denominator is always well away from zero.
    const double u = 2.0 * y / (seg.s0 + std::sqrt(seg.s0 * seg.s0 + 2.0 * seg.acceleration() * y));
    return seg.t0 + std::min(u, seg.duration);
}

} // namespace vmp

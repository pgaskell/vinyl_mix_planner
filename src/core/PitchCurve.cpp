#include "core/PitchCurve.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vmp
{

namespace
{
constexpr double kInfinity = std::numeric_limits<double>::infinity();

// Speed after travelling `distance` (source seconds) into a slide from `from` to `to` that
// covers `length` source seconds. The slide is linear in time, not in source position.
double speedPartWay(double from, double to, double length, double distance)
{
    const double duration = 2.0 * length / (from + to);
    const double acceleration = (to - from) / duration;
    const double u = 2.0 * distance / (from + std::sqrt(from * from + 2.0 * acceleration * distance));
    return from + acceleration * u;
}
} // namespace

PitchCurve::PitchCurve(double baseSpeed, std::vector<Point> points)
{
    std::stable_sort(points.begin(), points.end(),
                     [](const Point& a, const Point& b) { return a.sourceSeconds < b.sourceSeconds; });

    // Speed at the needle drop, and the points still ahead of it.
    double speed = baseSpeed;
    auto ahead = std::find_if(points.begin(), points.end(), [](const Point& p) { return p.sourceSeconds >= 0.0; });
    if (!points.empty())
    {
        if (ahead == points.begin())
            speed = ahead->speed;
        else if (ahead == points.end())
            speed = points.back().speed;
        else // the needle drop is part way through a slide
        {
            const auto& before = *std::prev(ahead);
            speed = speedPartWay(before.speed, ahead->speed, ahead->sourceSeconds - before.sourceSeconds,
                                 -before.sourceSeconds);
        }
    }

    double t = 0.0, x = 0.0;
    for (auto it = ahead; it != points.end(); ++it)
    {
        if (it->sourceSeconds > x)
        {
            const double duration = 2.0 * (it->sourceSeconds - x) / (speed + it->speed);
            segments_.push_back({t, duration, speed, it->speed, x, (it->speed - speed) / duration});
            t += duration;
            x = it->sourceSeconds;
        }
        speed = it->speed; // a point at the same position as the last is a jump
    }
    segments_.push_back({t, kInfinity, speed, speed, x, 0.0});
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
    return seg.s0 + seg.acceleration * (t - seg.t0);
}

double PitchCurve::sourceSecondsAt(double t) const
{
    // Before the needle drop, extrapolate at the starting speed (useful for lead-in drawing).
    if (t < 0.0)
        return segments_.front().s0 * t;
    const auto& seg = segmentAtTime(t);
    const double u = t - seg.t0;
    return seg.x0 + seg.s0 * u + 0.5 * seg.acceleration * u * u;
}

double PitchCurve::timeAtSourceSeconds(double x) const
{
    if (x < 0.0)
        return x / segments_.front().s0;
    const auto& seg = segmentAtSource(x);
    const double y = x - seg.x0;
    // Root of 0.5*a*u^2 + s0*u - y = 0, in the form that stays stable as a -> 0.
    // Speed never goes below 0.92, so the denominator is always well away from zero.
    const double u = 2.0 * y / (seg.s0 + std::sqrt(seg.s0 * seg.s0 + 2.0 * seg.acceleration * y));
    return seg.t0 + std::min(u, seg.duration);
}

} // namespace vmp

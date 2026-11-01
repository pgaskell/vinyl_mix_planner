#pragma once

#include <vector>

namespace vmp
{

// Playback speed over the life of a clip, and the mapping between real (timeline) time and
// position in the source audio. Time t is seconds since the needle drop; source position x is
// seconds of source audio played since the needle drop. x(t) is the integral of speed(t).
//
// With no points the speed is constant. Otherwise each point sets the speed at a position in
// the music, and the fader slides linearly in time between consecutive points; before the first
// point the speed is the first point's, after the last it is the last's. Points are anchored to
// the music, so trimming or sliding the clip keeps them on the same bar. Speed is piecewise
// linear, so x(t) is piecewise quadratic and both directions of the mapping are closed form.
class PitchCurve
{
public:
    struct Point
    {
        double sourceSeconds; // relative to the needle drop; may be negative (trimmed away)
        double speed;
    };

    // Points may be in any order. Two points at the same position make a jump.
    explicit PitchCurve(double baseSpeed = 1.0, std::vector<Point> points = {});

    double speedAt(double t) const;
    double sourceSecondsAt(double t) const;
    double timeAtSourceSeconds(double x) const;

private:
    struct Segment
    {
        double t0;           // start time
        double duration;     // infinite for the final hold
        double s0, s1;       // speed at start and end (equal for a hold)
        double x0;           // source position at t0
        double acceleration; // (s1 - s0) / duration; 0 for a hold
    };

    const Segment& segmentAtTime(double t) const;
    const Segment& segmentAtSource(double x) const;

    std::vector<Segment> segments_; // never empty
};

} // namespace vmp

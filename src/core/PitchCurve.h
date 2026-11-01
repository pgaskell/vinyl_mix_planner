#pragma once

#include <vector>

namespace vmp
{

// Playback speed over the life of a clip, and the mapping between real (timeline) time and
// position in the source audio. Time t is seconds since the needle drop; source position x is
// seconds of source audio played since the needle drop. x(t) is the integral of speed(t).
//
// The fader sits at a base pitch and can be moved during the clip. Each move is anchored to the
// music (a source position) rather than to the timeline, so trimming or sliding the clip keeps
// the move on the same bar. Speed is piecewise linear, so x(t) is piecewise quadratic and both
// directions of the mapping are closed form.
class PitchCurve
{
public:
    struct Move
    {
        double sourceSeconds;   // where in the music the move starts, relative to the needle drop
        double durationSeconds; // real time the fader takes to get there; 0 is a jump
        int targetSteps;
    };

    // Moves may be in any order. Moves anchored before the needle drop are ignored. Throws
    // std::invalid_argument if a move has a negative duration or starts before the previous one
    // has finished.
    explicit PitchCurve(int baseSteps = 0, std::vector<Move> moves = {});

    double speedAt(double t) const;
    double sourceSecondsAt(double t) const;
    double timeAtSourceSeconds(double x) const;

private:
    struct Segment
    {
        double t0;       // start time
        double duration; // infinite for the final hold
        double s0, s1;   // speed at start and end (equal for a hold)
        double x0;       // source position at t0
        double acceleration() const { return s1 == s0 ? 0.0 : (s1 - s0) / duration; }
    };

    const Segment& segmentAtTime(double t) const;
    const Segment& segmentAtSource(double x) const;

    std::vector<Segment> segments_; // never empty
};

} // namespace vmp

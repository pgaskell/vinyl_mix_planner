#pragma once

#include "engine/Glitches.h"
#include "engine/Schedule.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace vmp
{

struct BounceRange
{
    double start = 0.0, end = 0.0; // timeline seconds
};

// The whole set: from the first needle drop to the end of the last clip. Nothing for an empty set.
std::optional<BounceRange> bounceRange(const Schedule& schedule);

struct BounceResult
{
    enum class Status
    {
        Done,
        Cancelled,
        WriteFailed,
        MissingAudio, // a clip's audio isn't loaded (still decoding, or failed): nothing rendered
        Glitched      // finished, but audio wasn't read in time somewhere: the file has gaps
    };
    Status status = Status::Done;
    std::vector<Glitch> glitches;
    int64_t frames = 0;         // rendered
    float peak = 0.0f;          // largest sample, 1 = full scale
    int64_t clippedSamples = 0; // beyond full scale (a fixed-point file clips them)
};

// Renders the internal mix of `schedule` over `range` at `sampleRate`, through an Engine and a
// Prefetcher of its own: the same code as live playback, the set's own fader, EQ and automation
// (not a hardware mixer's live controls). Streamed tracks are reopened, so the live engine keeps
// playing undisturbed.
//
// `write` gets each block, left and right; returning false stops with WriteFailed. `progress` gets
// the fraction done now and then; returning false cancels. Reads files: call it on a background
// thread.
BounceResult bounce(const Schedule& schedule, double sampleRate, BounceRange range,
                    const std::function<bool(const float* const* channels, int numFrames)>& write,
                    const std::function<bool(double fraction)>& progress);

} // namespace vmp

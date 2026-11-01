#pragma once

#include "core/TrackAudio.h"

#include <array>
#include <cstdint>
#include <vector>

namespace vmp
{

// Mono 3-band waveform (as on CDJ-3000s): the peak level of the low, mid and high bands at
// several resolutions, for drawing at any zoom. Levels are stored as bytes to keep memory
// down (~3 MB per 6-minute track at the finest level).
class WaveformPeaks
{
public:
    static constexpr double kLowMidHz = 200.0;
    static constexpr double kMidHighHz = 2500.0;

    struct Bands
    {
        float low = 0.0f, mid = 0.0f, high = 0.0f; // peak absolute level, 0..1
    };

    explicit WaveformPeaks(const TrackAudio& audio);

    // Peak levels over frames [first, last). Ranges shorter than the finest bin (16 frames)
    // return that bin.
    Bands range(int64_t first, int64_t last) const;

private:
    using Bin = std::array<uint8_t, 3>;
    struct Level
    {
        int64_t binFrames;
        std::vector<Bin> bins;
    };

    int64_t frames_;
    std::vector<Level> levels_; // finest first
};

} // namespace vmp

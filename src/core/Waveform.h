#pragma once

#include "core/TrackAudio.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
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

    // A small binary file, so reopening a set doesn't need the audio decoded again. load() returns
    // nothing if the file is missing, unreadable, from another version or for a different length.
    bool save(const std::filesystem::path& file) const;
    static std::optional<WaveformPeaks> load(const std::filesystem::path& file, int64_t expectedFrames);

    // Peak levels over frames [first, last). Ranges shorter than the finest bin (16 frames)
    // return that bin.
    Bands range(int64_t first, int64_t last) const;

private:
    WaveformPeaks() = default;

    using Bin = std::array<uint8_t, 3>;
    struct Level
    {
        int64_t binFrames;
        std::vector<Bin> bins;
    };

    int64_t frames_ = 0;
    std::vector<Level> levels_; // finest first
};

} // namespace vmp

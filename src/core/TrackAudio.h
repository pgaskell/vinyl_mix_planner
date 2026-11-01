#pragma once

#include <cstdint>
#include <vector>

namespace vmp
{

// Decoded audio for one track: interleaved stereo float, immutable once shared.
// Whole file in memory (~20 MB per minute at 44.1 kHz). Fine for a handful of tracks; a long
// set needs disk streaming, which is planned but not built.
struct TrackAudio
{
    std::vector<float> interleaved;
    int64_t frames = 0;
    double sampleRate = 44100.0;
};

} // namespace vmp

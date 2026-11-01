#pragma once

#include "core/TrackAudio.h"

#include <memory>
#include <utility>
#include <vector>

namespace vmp
{

// Min/max summaries of a track at several resolutions, for drawing waveforms at any zoom.
// Below the finest level, range() reads the samples directly.
class WaveformPeaks
{
public:
    explicit WaveformPeaks(std::shared_ptr<const TrackAudio> audio);

    // Lowest and highest sample over frames [first, last), across both channels.
    std::pair<float, float> range(int64_t first, int64_t last) const;

private:
    struct Level
    {
        int64_t binFrames;
        std::vector<float> mins, maxs;
    };

    std::shared_ptr<const TrackAudio> audio_;
    std::vector<Level> levels_; // finest first
};

} // namespace vmp

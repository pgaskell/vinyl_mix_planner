#include "core/Waveform.h"

#include "core/Filters.h"

#include <algorithm>
#include <cmath>

namespace vmp
{

namespace
{
constexpr int64_t kFinestBin = 16;
constexpr int64_t kLevelFactor = 8;
constexpr int kNumLevels = 5; // 16, 128, 1024, 8192, 65536 frames per bin

uint8_t toByte(float level)
{
    return static_cast<uint8_t>(std::lround(std::clamp(level, 0.0f, 1.0f) * 255.0f));
}
} // namespace

WaveformPeaks::WaveformPeaks(const TrackAudio& audio) : frames_(audio.frames)
{
    ThreeBandSplitter splitter;
    splitter.design(kLowMidHz, kMidHighHz, audio.sampleRate);

    Level finest{kFinestBin, std::vector<Bin>(static_cast<size_t>((frames_ + kFinestBin - 1) / kFinestBin))};
    const float* samples = audio.interleaved.data();
    std::array<float, 3> peak{};
    for (int64_t f = 0; f < frames_; ++f)
    {
        float low, mid, high;
        splitter.process(0.5f * (samples[f * 2] + samples[f * 2 + 1]), low, mid, high);
        peak[0] = std::max(peak[0], std::abs(low));
        peak[1] = std::max(peak[1], std::abs(mid));
        peak[2] = std::max(peak[2], std::abs(high));
        if ((f + 1) % kFinestBin == 0 || f + 1 == frames_)
        {
            finest.bins[static_cast<size_t>(f / kFinestBin)] = {toByte(peak[0]), toByte(peak[1]), toByte(peak[2])};
            peak = {};
        }
    }
    levels_.push_back(std::move(finest));

    for (int level = 1; level < kNumLevels; ++level)
    {
        const auto& finer = levels_.back().bins;
        Level coarser{levels_.back().binFrames * kLevelFactor,
                      std::vector<Bin>((finer.size() + kLevelFactor - 1) / kLevelFactor)};
        for (size_t i = 0; i < finer.size(); ++i)
        {
            auto& bin = coarser.bins[i / kLevelFactor];
            for (size_t b = 0; b < 3; ++b)
                bin[b] = std::max(bin[b], finer[i][b]);
        }
        levels_.push_back(std::move(coarser));
    }
}

WaveformPeaks::Bands WaveformPeaks::range(int64_t first, int64_t last) const
{
    first = std::clamp<int64_t>(first, 0, frames_);
    last = std::clamp<int64_t>(last, 0, frames_);
    if (last <= first)
        return {};

    // Coarsest level whose bins still fit inside the range; edges round out to whole bins.
    const int64_t length = last - first;
    const Level* level = &levels_.front();
    for (const auto& l : levels_)
        if (l.binFrames <= length)
            level = &l;

    const auto firstBin = static_cast<size_t>(first / level->binFrames);
    const auto lastBin =
        std::min(static_cast<size_t>((last + level->binFrames - 1) / level->binFrames), level->bins.size());
    Bin peak{};
    for (size_t i = firstBin; i < lastBin; ++i)
        for (size_t b = 0; b < 3; ++b)
            peak[b] = std::max(peak[b], level->bins[i][b]);
    return {peak[0] / 255.0f, peak[1] / 255.0f, peak[2] / 255.0f};
}

} // namespace vmp

#include "core/Waveform.h"

#include <algorithm>

namespace vmp
{

namespace
{
constexpr int64_t kFinestBin = 64;
constexpr int64_t kLevelFactor = 8;
constexpr int kNumLevels = 4; // 64, 512, 4096, 32768 frames per bin
} // namespace

WaveformPeaks::WaveformPeaks(std::shared_ptr<const TrackAudio> audio) : audio_(std::move(audio))
{
    const float* samples = audio_->interleaved.data();
    int64_t bin = kFinestBin;
    for (int level = 0; level < kNumLevels; ++level, bin *= kLevelFactor)
    {
        Level l{bin, {}, {}};
        const auto count = static_cast<size_t>((audio_->frames + bin - 1) / bin);
        l.mins.resize(count);
        l.maxs.resize(count);

        for (size_t i = 0; i < count; ++i)
        {
            float lo = 0.0f, hi = 0.0f;
            if (level == 0)
            {
                const int64_t first = static_cast<int64_t>(i) * bin;
                const int64_t last = std::min(first + bin, audio_->frames);
                lo = hi = samples[first * 2];
                for (int64_t f = first * 2; f < last * 2; ++f)
                {
                    lo = std::min(lo, samples[f]);
                    hi = std::max(hi, samples[f]);
                }
            }
            else
            {
                // Built from the level below.
                const auto& finer = levels_.back();
                const size_t first = i * static_cast<size_t>(kLevelFactor);
                const size_t last = std::min(first + static_cast<size_t>(kLevelFactor), finer.mins.size());
                lo = *std::min_element(finer.mins.begin() + static_cast<long>(first),
                                       finer.mins.begin() + static_cast<long>(last));
                hi = *std::max_element(finer.maxs.begin() + static_cast<long>(first),
                                       finer.maxs.begin() + static_cast<long>(last));
            }
            l.mins[i] = lo;
            l.maxs[i] = hi;
        }
        levels_.push_back(std::move(l));
    }
}

std::pair<float, float> WaveformPeaks::range(int64_t first, int64_t last) const
{
    first = std::clamp<int64_t>(first, 0, audio_->frames);
    last = std::clamp<int64_t>(last, 0, audio_->frames);
    if (last <= first)
        return {0.0f, 0.0f};

    const int64_t length = last - first;
    if (length < kFinestBin)
    {
        const float* samples = audio_->interleaved.data();
        float lo = samples[first * 2], hi = lo;
        for (int64_t f = first * 2; f < last * 2; ++f)
        {
            lo = std::min(lo, samples[f]);
            hi = std::max(hi, samples[f]);
        }
        return {lo, hi};
    }

    // Coarsest level whose bins still fit inside the range; edges round out to whole bins.
    const Level* level = &levels_.front();
    for (const auto& l : levels_)
        if (l.binFrames <= length)
            level = &l;

    const auto firstBin = static_cast<size_t>(first / level->binFrames);
    const auto lastBin = std::min(static_cast<size_t>((last + level->binFrames - 1) / level->binFrames),
                                  level->mins.size());
    float lo = level->mins[firstBin], hi = level->maxs[firstBin];
    for (size_t b = firstBin + 1; b < lastBin; ++b)
    {
        lo = std::min(lo, level->mins[b]);
        hi = std::max(hi, level->maxs[b]);
    }
    return {lo, hi};
}

} // namespace vmp

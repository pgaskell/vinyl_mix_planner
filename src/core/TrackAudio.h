#pragma once

#include "core/LargeAllocator.h"
#include "core/StreamedAudio.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

namespace vmp
{

// Decoded audio for one track, interleaved stereo float, immutable once shared. Either held in
// memory (~20 MB per minute at 44.1 kHz: tests, analysis, and the fallback when the decode cache
// can't be written) or streamed from the decode cache, where only the chunks the prefetcher has
// loaded are available.
struct TrackAudio
{
    std::vector<float, LargeAllocator<float>> interleaved; // in memory; empty when streamed
    int64_t frames = 0;
    double sampleRate = 44100.0;
    std::shared_ptr<StreamedAudio> stream;

    // In-memory audio only.
    const float* data() const { return interleaved.data(); }

    // Real-time safe. The samples from `frame` on and how many frames follow contiguously, or null
    // if that part isn't loaded (or is outside the track).
    const float* framesAt(int64_t frame, int64_t& count) const
    {
        count = 0;
        if (frame < 0 || frame >= frames)
            return nullptr;
        if (stream == nullptr)
        {
            count = frames - frame;
            return interleaved.data() + frame * 2;
        }
        const int64_t index = frame / StreamedAudio::kChunkFrames;
        const float* chunk = stream->chunk(index);
        if (chunk == nullptr)
            return nullptr;
        const int64_t offset = frame - index * StreamedAudio::kChunkFrames;
        count = stream->chunkFrames(index) - offset;
        return chunk + offset * 2;
    }

    // Real-time safe. Whether every frame in [first, last) that lies inside the track is loaded.
    bool resident(int64_t first, int64_t last) const
    {
        if (stream == nullptr)
            return true;
        first = std::max<int64_t>(first, 0);
        last = std::min(last, frames);
        for (int64_t i = first / StreamedAudio::kChunkFrames; first < last && i <= (last - 1) / StreamedAudio::kChunkFrames;
             ++i)
            if (stream->chunk(i) == nullptr)
                return false;
        return true;
    }
};

} // namespace vmp

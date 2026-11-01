#pragma once

#include "engine/Deck.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace vmp::test
{

inline TrackAudio* makeSine(double frequency, double sampleRate, double seconds)
{
    auto* track = new TrackAudio;
    track->sampleRate = sampleRate;
    track->frames = static_cast<int64_t>(seconds * sampleRate);
    track->interleaved.resize(static_cast<size_t>(track->frames) * 2);
    for (int64_t i = 0; i < track->frames; ++i)
    {
        const auto v = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * frequency * i / sampleRate));
        track->interleaved[static_cast<size_t>(i) * 2] = v;
        track->interleaved[static_cast<size_t>(i) * 2 + 1] = v;
    }
    return track;
}

inline TrackAudio* makeConstant(float value, double sampleRate, double seconds)
{
    auto* track = new TrackAudio;
    track->sampleRate = sampleRate;
    track->frames = static_cast<int64_t>(seconds * sampleRate);
    track->interleaved.assign(static_cast<size_t>(track->frames) * 2, value);
    return track;
}

// Frequency from interpolated rising zero crossings.
inline double measureFrequency(const std::vector<float>& signal, double sampleRate)
{
    double first = -1.0, last = -1.0;
    int crossings = 0;
    for (size_t i = 1; i < signal.size(); ++i)
    {
        if (signal[i - 1] < 0.0f && signal[i] >= 0.0f)
        {
            const double t = static_cast<double>(i - 1) + signal[i - 1] / (signal[i - 1] - signal[i]);
            if (first < 0.0)
                first = t;
            last = t;
            ++crossings;
        }
    }
    if (crossings < 2)
        return 0.0;
    return (crossings - 1) / ((last - first) / sampleRate);
}

// Renders a deck in fixed blocks, returning the left channel.
inline std::vector<float> renderDeck(Deck& deck, int frames, int blockSize = 512)
{
    std::vector<float> left(static_cast<size_t>(frames)), right(static_cast<size_t>(frames));
    for (int done = 0; done < frames; done += blockSize)
    {
        const int n = std::min(blockSize, frames - done);
        deck.render(left.data() + done, right.data() + done, n);
    }
    return left;
}

} // namespace vmp::test

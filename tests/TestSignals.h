#pragma once

#include "core/TrackAudio.h"
#include "engine/Deck.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <vector>

namespace vmp::test
{

inline std::shared_ptr<TrackAudio> makeTrack(double sampleRate, double seconds)
{
    auto track = std::make_shared<TrackAudio>();
    track->sampleRate = sampleRate;
    track->frames = static_cast<int64_t>(seconds * sampleRate);
    track->interleaved.assign(static_cast<size_t>(track->frames) * 2, 0.0f);
    return track;
}

inline std::shared_ptr<TrackAudio> makeSine(double frequency, double sampleRate, double seconds,
                                            double amplitude = 0.5)
{
    auto track = makeTrack(sampleRate, seconds);
    for (int64_t i = 0; i < track->frames; ++i)
    {
        const auto v = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * frequency * i / sampleRate));
        track->interleaved[static_cast<size_t>(i) * 2] = v;
        track->interleaved[static_cast<size_t>(i) * 2 + 1] = v;
    }
    return track;
}

inline std::shared_ptr<TrackAudio> makeConstant(float value, double sampleRate, double seconds)
{
    auto track = makeTrack(sampleRate, seconds);
    std::fill(track->interleaved.begin(), track->interleaved.end(), value);
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

inline float peak(const std::vector<float>& signal, size_t first = 0, size_t last = SIZE_MAX)
{
    float p = 0.0f;
    for (size_t i = first; i < std::min(last, signal.size()); ++i)
        p = std::max(p, std::abs(signal[i]));
    return p;
}

inline std::vector<float> slice(const std::vector<float>& signal, double fromSeconds, double toSeconds,
                                double sampleRate)
{
    const auto first = static_cast<size_t>(fromSeconds * sampleRate);
    const auto last = std::min(static_cast<size_t>(toSeconds * sampleRate), signal.size());
    return {signal.begin() + static_cast<long>(first), signal.begin() + static_cast<long>(last)};
}

} // namespace vmp::test

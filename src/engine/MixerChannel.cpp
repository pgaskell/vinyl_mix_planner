#include "engine/MixerChannel.h"

#include <algorithm>
#include <cmath>

namespace vmp
{

float MixerChannel::eqGain(double knob)
{
    knob = std::clamp(knob, 0.0, 1.0);
    if (knob <= 0.5)
        return static_cast<float>(std::pow(knob * 2.0, 3.0)); // -18 dB at a quarter turn, kill at 0
    return static_cast<float>(std::pow(10.0, (knob - 0.5) * 2.0 * 6.0 / 20.0));
}

float MixerChannel::faderGain(double position)
{
    position = std::clamp(position, 0.0, 1.0);
    return static_cast<float>(position * position); // -12 dB at half way
}

void MixerChannel::prepare(double sampleRate)
{
    for (auto& b : bands_)
        b.design(kLowMidCrossoverHz, kMidHighCrossoverHz, sampleRate);
    reset();
}

void MixerChannel::reset()
{
    for (auto& b : bands_)
        b.reset();
}

void MixerChannel::process(const ChannelSettings& settings, float* left, float* right, int numFrames)
{
    const std::array<float, 4> target{eqGain(settings.low), eqGain(settings.mid), eqGain(settings.high),
                                      faderGain(settings.fader)};
    std::array<float, 4> step{};
    for (size_t i = 0; i < 4; ++i)
        step[i] = (target[i] - applied_[i]) / static_cast<float>(std::max(numFrames, 1));

    float* channels[2] = {left, right};
    for (size_t ch = 0; ch < 2; ++ch)
    {
        auto g = applied_;
        float* data = channels[ch];
        for (int i = 0; i < numFrames; ++i)
        {
            for (size_t k = 0; k < 4; ++k)
                g[k] += step[k];
            float low, mid, high;
            bands_[ch].process(data[i], low, mid, high);
            data[i] = (g[0] * low + g[1] * mid + g[2] * high) * g[3];
        }
    }
    applied_ = target;
}

} // namespace vmp

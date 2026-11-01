#include "engine/MixerChannel.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vmp
{

namespace
{
enum class FilterType
{
    LowPass,
    HighPass
};

// RBJ cookbook Butterworth (Q = 1/sqrt 2) coefficients.
template <typename Biquad>
void design(Biquad& f, FilterType type, double frequency, double sampleRate)
{
    const double w = 2.0 * std::numbers::pi * frequency / sampleRate;
    constexpr double q = 1.0 / std::numbers::sqrt2;
    const double alpha = std::sin(w) / (2.0 * q);
    const double cosw = std::cos(w);
    const double a0 = 1.0 + alpha;
    const double b1 = type == FilterType::LowPass ? 1.0 - cosw : -(1.0 + cosw);
    const double b0 = type == FilterType::LowPass ? b1 / 2.0 : -b1 / 2.0;
    f.b0 = static_cast<float>(b0 / a0);
    f.b1 = static_cast<float>(b1 / a0);
    f.b2 = static_cast<float>(b0 / a0);
    f.a1 = static_cast<float>(-2.0 * cosw / a0);
    f.a2 = static_cast<float>((1.0 - alpha) / a0);
}

template <typename LR4>
void designLR4(LR4& f, FilterType type, double frequency, double sampleRate)
{
    design(f.a, type, frequency, sampleRate);
    design(f.b, type, frequency, sampleRate);
}
} // namespace

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
    {
        designLR4(b.low1, FilterType::LowPass, kLowMidCrossoverHz, sampleRate);
        designLR4(b.high1, FilterType::HighPass, kLowMidCrossoverHz, sampleRate);
        designLR4(b.low2, FilterType::LowPass, kMidHighCrossoverHz, sampleRate);
        designLR4(b.high2, FilterType::HighPass, kMidHighCrossoverHz, sampleRate);
        designLR4(b.lowAllpassLow, FilterType::LowPass, kMidHighCrossoverHz, sampleRate);
        designLR4(b.lowAllpassHigh, FilterType::HighPass, kMidHighCrossoverHz, sampleRate);
    }
    reset();
}

void MixerChannel::reset()
{
    for (auto& b : bands_)
        for (LR4* f : {&b.low1, &b.high1, &b.low2, &b.high2, &b.lowAllpassLow, &b.lowAllpassHigh})
            f->a.z1 = f->a.z2 = f->b.z1 = f->b.z2 = 0.0f;
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
        auto& b = bands_[ch];
        auto g = applied_;
        float* data = channels[ch];
        for (int i = 0; i < numFrames; ++i)
        {
            for (size_t k = 0; k < 4; ++k)
                g[k] += step[k];
            const float x = data[i];
            const float lowRaw = b.low1.process(x);
            const float upper = b.high1.process(x);
            const float low = b.lowAllpassLow.process(lowRaw) + b.lowAllpassHigh.process(lowRaw);
            const float mid = b.low2.process(upper);
            const float high = b.high2.process(upper);
            data[i] = (g[0] * low + g[1] * mid + g[2] * high) * g[3];
        }
    }
    applied_ = target;
}

} // namespace vmp

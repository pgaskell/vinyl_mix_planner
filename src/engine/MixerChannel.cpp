#include "engine/MixerChannel.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vmp
{

float MixerChannel::eqGain(double knob)
{
    knob = std::clamp(knob, 0.0, 1.0);
    if (knob <= 0.5)
        return static_cast<float>(std::pow(knob * 2.0, 3.0)); // -18 dB at a quarter turn, kill at 0
    return static_cast<float>(std::pow(10.0, (knob - 0.5) * 2.0 * 6.0 / 20.0));
}

float MixerChannel::trimGain(double knob)
{
    knob = std::clamp(knob, 0.0, 1.0);
    return static_cast<float>(std::pow(10.0, (knob - 0.5) * 2.0 * 9.0 / 20.0));
}

float MixerChannel::faderGain(double position)
{
    position = std::clamp(position, 0.0, 1.0);
    return static_cast<float>(position * position); // -12 dB at half way
}

std::optional<MixerChannel::ColorFilter> MixerChannel::colorFilter(double knob)
{
    knob = std::clamp(knob, 0.0, 1.0);
    if (knob == 0.5)
        return std::nullopt;
    const double turn = std::abs(knob - 0.5) * 2.0; // 0 at the centre, 1 at either end
    // Resonance comes in over the first quarter of the turn, so a filter just off centre is flat.
    const double q = 1.0 / std::numbers::sqrt2 + (kColorResonantQ - 1.0 / std::numbers::sqrt2) * std::min(1.0, turn * 4.0);
    if (knob < 0.5)
        return ColorFilter{FilterType::LowPass, 20000.0 * std::pow(kColorLowPassMinHz / 20000.0, turn), q};
    return ColorFilter{FilterType::HighPass, 20.0 * std::pow(kColorHighPassMaxHz / 20.0, turn), q};
}

void MixerChannel::prepare(double sampleRate)
{
    sampleRate_ = sampleRate;
    for (auto& b : bands_)
        b.design(kLowMidCrossoverHz, kMidHighCrossoverHz, sampleRate);
    reset();
}

void MixerChannel::reset()
{
    for (auto& b : bands_)
        b.reset();
    for (auto& f : color_)
        f.reset();
}

void MixerChannel::processColor(double from, double to, float* left, float* right, int numFrames)
{
    // The knob glides across the block, the filter redesigned every few frames.
    constexpr int kStep = 16;
    std::optional<FilterType> last = colorFilter(from) ? std::optional(colorFilter(from)->type) : std::nullopt;
    for (int start = 0; start < numFrames; start += kStep)
    {
        const int n = std::min(kStep, numFrames - start);
        const double knob = from + (to - from) * static_cast<double>(start + n) / static_cast<double>(numFrames);
        const auto filter = colorFilter(knob);
        if (!filter)
        {
            last.reset();
            continue;
        }
        // Coming in from off, or flipping sides: start the filter settled on the signal. Near the
        // centre either filter passes nearly everything, so the output carries on smoothly.
        if (!last || *last != filter->type)
        {
            color_[0].settle(left[start], filter->type);
            color_[1].settle(right[start], filter->type);
        }
        last = filter->type;
        for (auto& f : color_)
            f.design(filter->hz, filter->q, sampleRate_);
        for (int i = start; i < start + n; ++i)
        {
            left[i] = color_[0].process(left[i], filter->type);
            right[i] = color_[1].process(right[i], filter->type);
        }
    }
}

void MixerChannel::process(const ChannelSettings& settings, float* left, float* right, int numFrames)
{
    const std::array<float, 5> target{eqGain(settings.low), eqGain(settings.mid), eqGain(settings.high),
                                      faderGain(settings.fader), trimGain(settings.trim)};
    std::array<float, 5> step{};
    for (size_t i = 0; i < 5; ++i)
        step[i] = (target[i] - applied_[i]) / static_cast<float>(std::max(numFrames, 1));

    float* channels[2] = {left, right};
    for (size_t ch = 0; ch < 2; ++ch)
    {
        auto g = applied_;
        float* data = channels[ch];
        for (int i = 0; i < numFrames; ++i)
        {
            for (size_t k = 0; k < 5; ++k)
                g[k] += step[k];
            float low, mid, high;
            bands_[ch].process(data[i] * g[4], low, mid, high);
            data[i] = (g[0] * low + g[1] * mid + g[2] * high) * g[3];
        }
    }
    applied_ = target;

    const double color = std::clamp(settings.color, 0.0, 1.0);
    if (color != 0.5 || appliedColor_ != 0.5)
        processColor(appliedColor_, color, left, right, numFrames);
    appliedColor_ = color;
}

} // namespace vmp

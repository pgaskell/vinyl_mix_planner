#pragma once

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <numbers>

namespace vmp
{

enum class FilterType
{
    LowPass,
    HighPass
};

// Transposed direct form II biquad.
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;

    // RBJ cookbook Butterworth (Q = 1/sqrt 2).
    void design(FilterType type, double frequency, double sampleRate)
    {
        constexpr double q = 1.0 / std::numbers::sqrt2;
        const double w = 2.0 * std::numbers::pi * frequency / sampleRate;
        const double alpha = std::sin(w) / (2.0 * q);
        const double cosw = std::cos(w);
        const double a0 = 1.0 + alpha;
        const double n1 = type == FilterType::LowPass ? 1.0 - cosw : -(1.0 + cosw);
        const double n0 = type == FilterType::LowPass ? n1 / 2.0 : -n1 / 2.0;
        b0 = static_cast<float>(n0 / a0);
        b1 = static_cast<float>(n1 / a0);
        b2 = static_cast<float>(n0 / a0);
        a1 = static_cast<float>(-2.0 * cosw / a0);
        a2 = static_cast<float>((1.0 - alpha) / a0);
    }

    void reset() { z1 = z2 = 0.0f; }

    float process(float x)
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// 4th-order Linkwitz-Riley: two identical Butterworths in series. Its low and high outputs
// sum to a flat (all-pass) response.
struct LR4
{
    Biquad a, b;

    void design(FilterType type, double frequency, double sampleRate)
    {
        a.design(type, frequency, sampleRate);
        b.design(type, frequency, sampleRate);
    }
    void reset()
    {
        a.reset();
        b.reset();
    }
    float process(float x) { return b.process(a.process(x)); }
};

// Splits a signal into low, mid and high bands that sum back to a flat response: split at f1,
// split the upper part at f2, and pass the low band through the f2 crossover too so all three
// bands share the same phase.
class ThreeBandSplitter
{
public:
    void design(double lowMidHz, double midHighHz, double sampleRate)
    {
        low1_.design(FilterType::LowPass, lowMidHz, sampleRate);
        high1_.design(FilterType::HighPass, lowMidHz, sampleRate);
        low2_.design(FilterType::LowPass, midHighHz, sampleRate);
        high2_.design(FilterType::HighPass, midHighHz, sampleRate);
        lowAllpassLow_.design(FilterType::LowPass, midHighHz, sampleRate);
        lowAllpassHigh_.design(FilterType::HighPass, midHighHz, sampleRate);
    }

    void reset()
    {
        for (LR4* f : {&low1_, &high1_, &low2_, &high2_, &lowAllpassLow_, &lowAllpassHigh_})
            f->reset();
    }

    void process(float x, float& low, float& mid, float& high)
    {
        const float lowRaw = low1_.process(x);
        const float upper = high1_.process(x);
        low = lowAllpassLow_.process(lowRaw) + lowAllpassHigh_.process(lowRaw);
        mid = low2_.process(upper);
        high = high2_.process(upper);
    }

private:
    LR4 low1_, high1_, low2_, high2_, lowAllpassLow_, lowAllpassHigh_;
};

// 2-pole state-variable filter (Zavalishin's topology-preserving transform), which stays stable
// and quiet while its cutoff and Q are swept.
struct Svf
{
    float a1 = 1, a2 = 0, a3 = 0, k = 1.414f;
    float ic1 = 0, ic2 = 0;

    void design(double frequency, double q, double sampleRate)
    {
        const double g = std::tan(std::numbers::pi * std::min(frequency, 0.45 * sampleRate) / sampleRate);
        k = static_cast<float>(1.0 / q);
        a1 = static_cast<float>(1.0 / (1.0 + g * (g + k)));
        a2 = static_cast<float>(g) * a1;
        a3 = static_cast<float>(g) * a2;
    }

    void reset() { ic1 = ic2 = 0.0f; }
    // Settled as if `x` had been the input for a while, so switching the filter in doesn't click:
    // a low-pass then outputs x, a high-pass 0 (from silence).
    void settle(float x, FilterType type)
    {
        ic1 = 0.0f;
        ic2 = type == FilterType::LowPass ? x : 0.0f;
    }

    float process(float x, FilterType type)
    {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return type == FilterType::LowPass ? v2 : x - k * v1 - v2;
    }
};

} // namespace vmp

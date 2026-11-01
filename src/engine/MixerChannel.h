#pragma once

#include "core/Set.h"

#include <array>

namespace vmp
{

// Basic mixer channel: 3-band isolator and channel fader, from knob positions.
// A placeholder until the DJM-900NXS2 emulation; its curves and crossovers are not measured.
//
// The isolator splits the signal with 4th-order Linkwitz-Riley crossovers and recombines the
// bands with their gains, so all knobs at centre is flat and a knob at zero kills its band.
class MixerChannel
{
public:
    static constexpr double kLowMidCrossoverHz = 300.0;
    static constexpr double kMidHighCrossoverHz = 3000.0;

    // Knob position (0..1, centre 0.5) to linear gain: 0 kills, centre is unity, full is +6 dB.
    static float eqGain(double knob);
    // Fader position (0..1) to linear gain.
    static float faderGain(double position);

    // Not real-time.
    void prepare(double sampleRate);
    void reset();

    // Audio thread. Gains glide from the previous block's values to avoid zipper noise.
    void process(const ChannelSettings& settings, float* left, float* right, int numFrames);

private:
    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float z1 = 0, z2 = 0;
        float process(float x)
        {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    // A 4th-order Linkwitz-Riley filter is two identical 2nd-order Butterworths in series.
    struct LR4
    {
        Biquad a, b;
        float process(float x) { return b.process(a.process(x)); }
    };

    // Per audio channel: split at f1, split the upper part at f2, and pass the low band through
    // the f2 crossover too so all three bands share the same phase.
    struct Bands
    {
        LR4 low1, high1, low2, high2, lowAllpassLow, lowAllpassHigh;
    };

    std::array<Bands, 2> bands_;
    std::array<float, 4> applied_{1.0f, 1.0f, 1.0f, 1.0f}; // low, mid, high, fader
};

} // namespace vmp

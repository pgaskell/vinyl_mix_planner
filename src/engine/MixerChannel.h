#pragma once

#include "core/Filters.h"
#include "core/Set.h"

#include <array>

namespace vmp
{

// Basic mixer channel: 3-band isolator and channel fader, from knob positions.
// A placeholder until the DJM-900NXS2 emulation; its curves and crossovers are not measured.
// The isolator recombines Linkwitz-Riley bands with their gains, so all knobs at centre is flat
// and a knob at zero kills its band.
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
    std::array<ThreeBandSplitter, 2> bands_;
    std::array<float, 4> applied_{1.0f, 1.0f, 1.0f, 1.0f}; // low, mid, high, fader
};

} // namespace vmp

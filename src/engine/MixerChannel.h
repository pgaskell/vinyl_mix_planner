#pragma once

#include "core/Filters.h"
#include "core/Set.h"

#include <array>
#include <optional>

namespace vmp
{

// Basic mixer channel: trim, 3-band isolator, color filter and channel fader, from knob positions.
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
    // Trim knob position (0..1) to linear gain, linear in dB: -12 dB at the centre, 8 dB either
    // way. The headroom lets faders sit fully up, as on a club mixer, without two loud records
    // summing past full scale in the internal mix.
    static constexpr double kTrimCentreDb = -12.0;
    static constexpr double kTrimRangeDb = 8.0;
    static float trimGain(double knob);
    // Fader position (0..1) to linear gain.
    static float faderGain(double position);

    // A control's position (Volume, EQ or Color knob) after a trim amount, for trimming
    // automation. 0 changes nothing; a quarter turn of a knob is an amount of 0.25.
    // - Volume and EQ: the gain moves by kTrimDbPerTurn dB per turn, so a setting held through a
    //   record moves with it; a closed fader stays closed and a kill stays a kill.
    // - Color: the distance from centre is multiplied by 2^(kTrimColorOctaves * amount), so off
    //   stays off.
    // Results are clamped to the control's travel (EQ to +6 dB). Pitch isn't trimmed.
    static constexpr double kTrimDbPerTurn = 24.0;
    static constexpr double kTrimColorOctaves = 4.0;
    static double trimmed(Param param, double position, double amount);

    // The COLOR knob as a DJ filter: off at the centre; to the left a low-pass sweeping down from
    // 20 kHz, to the right a high-pass sweeping up from 20 Hz, getting resonant as it turns.
    struct ColorFilter
    {
        FilterType type;
        double hz, q;
    };
    static constexpr double kColorLowPassMinHz = 100.0;
    static constexpr double kColorHighPassMaxHz = 10000.0;
    static constexpr double kColorResonantQ = 2.0;
    static std::optional<ColorFilter> colorFilter(double knob);

    // Not real-time.
    void prepare(double sampleRate);
    void reset();

    // Audio thread. Gains glide from the previous block's values to avoid zipper noise.
    void process(const ChannelSettings& settings, float* left, float* right, int numFrames);

private:
    std::array<ThreeBandSplitter, 2> bands_;
    std::array<float, 5> applied_{1.0f, 1.0f, 1.0f, 1.0f, 1.0f}; // low, mid, high, fader, trim; set by prepare()
    void processColor(double from, double to, float* left, float* right, int numFrames);
    double sampleRate_ = 48000.0;
    std::array<Svf, 2> color_;
    double appliedColor_ = 0.5;
};

} // namespace vmp

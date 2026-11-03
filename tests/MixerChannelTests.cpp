#include "TestSignals.h"

#include "engine/MixerChannel.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace vmp;
using namespace vmp::test;
using Catch::Approx;

namespace
{
constexpr double kRate = 48000.0;

// Amplitude of a sine after the channel, once the filters have settled.
float responseAt(double frequency, const ChannelSettings& settings)
{
    MixerChannel channel;
    channel.prepare(kRate);
    const auto track = makeSine(frequency, kRate, 1.0, 1.0);
    std::vector<float> left(static_cast<size_t>(track->frames)), right(left.size());
    for (size_t i = 0; i < left.size(); ++i)
        left[i] = right[i] = track->interleaved[i * 2];
    for (size_t done = 0; done < left.size(); done += 512)
        channel.process(settings, left.data() + done, right.data() + done,
                        static_cast<int>(std::min<size_t>(512, left.size() - done)));

    // RMS over the settled second half: sample peaks under-read high frequencies by up to ~1 dB.
    double sum = 0.0;
    for (size_t i = left.size() / 2; i < left.size(); ++i)
        sum += static_cast<double>(left[i]) * left[i];
    return static_cast<float>(std::sqrt(2.0 * sum / static_cast<double>(left.size() / 2)));
}

double dB(float gain)
{
    return 20.0 * std::log10(std::max(gain, 1e-9f));
}
} // namespace

TEST_CASE("Isolator with centred knobs is flat")
{
    for (double f : {40.0, 150.0, 300.0, 1000.0, 3000.0, 8000.0, 15000.0})
    {
        INFO(f << " Hz");
        CHECK(dB(responseAt(f, {})) == Approx(0.0).margin(0.2));
    }
}

TEST_CASE("A knob at zero kills its band")
{
    CHECK(dB(responseAt(60.0, {1.0, 0.5, 0.5, 0.0})) < -40.0);
    CHECK(dB(responseAt(1000.0, {1.0, 0.5, 0.0, 0.5})) < -30.0);
    CHECK(dB(responseAt(12000.0, {1.0, 0.0, 0.5, 0.5})) < -40.0);
    // The other bands are untouched.
    CHECK(dB(responseAt(12000.0, {1.0, 0.5, 0.5, 0.0})) == Approx(0.0).margin(0.2));
}

TEST_CASE("Knob and fader laws")
{
    CHECK(MixerChannel::eqGain(0.0) == 0.0f);
    CHECK(MixerChannel::eqGain(0.5) == 1.0f);
    CHECK(dB(MixerChannel::eqGain(1.0)) == Approx(6.0).margin(0.01));
    CHECK(MixerChannel::faderGain(0.0) == 0.0f);
    CHECK(MixerChannel::faderGain(1.0) == 1.0f);
    CHECK(dB(MixerChannel::trimGain(0.0)) == Approx(-9.0).margin(0.01));
    CHECK(MixerChannel::trimGain(0.5) == 1.0f);
    CHECK(dB(MixerChannel::trimGain(0.75)) == Approx(4.5).margin(0.01));
    CHECK(dB(MixerChannel::trimGain(1.0)) == Approx(9.0).margin(0.01));
    CHECK(dB(responseAt(1000.0, {1.0, 0.5, 0.5, 0.5, 1.0})) == Approx(9.0).margin(0.2));
    CHECK(dB(responseAt(1000.0, {0.5, 0.5, 0.5, 0.5})) == Approx(-12.04).margin(0.2));
}

TEST_CASE("COLOR is off at the centre and sweeps a low-pass left, a high-pass right")
{
    CHECK_FALSE(MixerChannel::colorFilter(0.5));
    const auto left = MixerChannel::colorFilter(0.0);
    REQUIRE(left);
    CHECK(left->type == FilterType::LowPass);
    CHECK(left->hz == Approx(MixerChannel::kColorLowPassMinHz));
    CHECK(left->q == Approx(MixerChannel::kColorResonantQ));
    const auto right = MixerChannel::colorFilter(1.0);
    REQUIRE(right);
    CHECK(right->type == FilterType::HighPass);
    CHECK(right->hz == Approx(MixerChannel::kColorHighPassMaxHz));
    // Just off centre: wide open and not yet resonant.
    const auto nudge = MixerChannel::colorFilter(0.499);
    REQUIRE(nudge);
    CHECK(nudge->hz > 19000.0);
    CHECK(nudge->q < 0.75);

    ChannelSettings settings;
    for (double f : {40.0, 1000.0, 15000.0})
    {
        INFO(f << " Hz");
        CHECK(dB(responseAt(f, settings)) == Approx(0.0).margin(0.2));
    }
    settings.color = 0.0; // low-pass at 100 Hz
    CHECK(dB(responseAt(40.0, settings)) > -1.0);
    CHECK(dB(responseAt(1000.0, settings)) < -30.0);
    settings.color = 1.0; // high-pass at 10 kHz
    CHECK(dB(responseAt(15000.0, settings)) > -2.0);
    CHECK(dB(responseAt(300.0, settings)) < -40.0);
    settings.color = 0.25; // half way left: resonant peak at the cutoff
    const auto half = MixerChannel::colorFilter(0.25);
    CHECK(dB(responseAt(half->hz, settings)) == Approx(20.0 * std::log10(MixerChannel::kColorResonantQ)).margin(0.5));
}

TEST_CASE("Sweeping COLOR through the centre doesn't click")
{
    MixerChannel channel;
    channel.prepare(kRate);
    const auto track = makeSine(220.0, kRate, 4.0, 0.5);
    std::vector<float> left(static_cast<size_t>(track->frames)), right(left.size());
    for (size_t i = 0; i < left.size(); ++i)
        left[i] = right[i] = track->interleaved[i * 2];
    // Fast knob moves block by block: centre -> left end -> right end -> centre.
    const size_t blocks = left.size() / 512;
    float biggestStep = 0.0f;
    for (size_t b = 0; b < blocks; ++b)
    {
        const double t = static_cast<double>(b) / static_cast<double>(blocks);
        ChannelSettings settings;
        settings.color = t < 0.25 ? 0.5 - 2.0 * t : t < 0.75 ? 2.0 * t - 0.5 : 1.0 - 2.0 * (t - 0.75);
        channel.process(settings, left.data() + b * 512, right.data() + b * 512, 512);
        for (size_t i = std::max<size_t>(b * 512, 1); i < (b + 1) * 512; ++i)
        {
            REQUIRE(std::isfinite(left[i]));
            biggestStep = std::max(biggestStep, std::abs(left[i] - left[i - 1]));
        }
    }
    // A clean 220 Hz sine at 0.5 moves at most 0.0144 per sample; the resonance can double that.
    // A click is a jump many times bigger.
    CHECK(biggestStep < 0.04f);
}

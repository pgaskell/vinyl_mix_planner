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
    CHECK(dB(responseAt(1000.0, {0.5, 0.5, 0.5, 0.5})) == Approx(-12.04).margin(0.2));
}

#include "TestSignals.h"

#include "core/Analysis.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using namespace vmp;
using namespace vmp::test;
using Catch::Approx;

namespace
{
// A drum pattern: a kick on every beat and, optionally, a quieter hat on every off-beat.
std::shared_ptr<TrackAudio> makeBeat(double bpm, double firstBeat, double seconds, bool offbeatHats,
                                     double sampleRate = 44100.0)
{
    auto track = makeTrack(sampleRate, seconds);
    const double period = 60.0 / bpm;
    uint32_t noise = 12345;
    auto addHit = [&](double at, bool kick) {
        const auto start = static_cast<int64_t>(at * sampleRate);
        const auto length = static_cast<int64_t>((kick ? 0.15 : 0.04) * sampleRate);
        for (int64_t i = 0; i < length && start + i < track->frames; ++i)
        {
            const double t = static_cast<double>(i) / sampleRate;
            double v;
            if (kick)
                v = 0.8 * std::sin(2.0 * std::numbers::pi * 55.0 * t) * std::exp(-t * 25.0);
            else
            {
                noise = noise * 1664525u + 1013904223u;
                v = 0.25 * (static_cast<double>(noise) / 4294967295.0 * 2.0 - 1.0) * std::exp(-t * 120.0);
            }
            track->interleaved[static_cast<size_t>(start + i) * 2] += static_cast<float>(v);
            track->interleaved[static_cast<size_t>(start + i) * 2 + 1] += static_cast<float>(v);
        }
    };
    for (double t = firstBeat; t < seconds; t += period)
    {
        addHit(t, true);
        if (offbeatHats)
            addHit(t + period / 2.0, false);
    }
    return track;
}

// Sustained notes with a few harmonics.
std::shared_ptr<TrackAudio> makeChord(std::initializer_list<std::pair<double, double>> notes, double seconds)
{
    auto track = makeTrack(44100.0, seconds);
    for (int64_t i = 0; i < track->frames; ++i)
    {
        const double t = static_cast<double>(i) / 44100.0;
        double v = 0.0;
        for (const auto& [hz, level] : notes)
            for (int h = 1; h <= 3; ++h)
                v += level / h * std::sin(2.0 * std::numbers::pi * hz * h * t);
        track->interleaved[static_cast<size_t>(i) * 2] = track->interleaved[static_cast<size_t>(i) * 2 + 1] =
            static_cast<float>(0.1 * v);
    }
    return track;
}

// Distance between two beat phases, allowing for wrap-around.
double phaseError(double a, double b, double period)
{
    const double d = std::fmod(std::abs(a - b), period);
    return std::min(d, period - d);
}
} // namespace

TEST_CASE("Tempo and beat positions are found")
{
    for (const double bpm : {124.0, 128.0, 97.3})
    {
        INFO(bpm << " BPM");
        const auto analysis = analyseTrack(*makeBeat(bpm, 0.25, 60.0, true));
        REQUIRE(analysis.grid);
        CHECK(analysis.grid->bpm == Approx(bpm).margin(0.05));
        INFO("first beat " << analysis.grid->firstBeatSeconds);
        CHECK(phaseError(analysis.grid->firstBeatSeconds, 0.25, 60.0 / bpm) < 0.008);
    }
}

TEST_CASE("Off-beat hats don't double the tempo")
{
    const auto analysis = analyseTrack(*makeBeat(90.0, 0.1, 60.0, true));
    REQUIRE(analysis.grid);
    CHECK(analysis.grid->bpm == Approx(90.0).margin(0.05));
}

TEST_CASE("Analysis works at 48 kHz")
{
    const auto analysis = analyseTrack(*makeBeat(126.0, 0.4, 60.0, false, 48000.0));
    REQUIRE(analysis.grid);
    CHECK(analysis.grid->bpm == Approx(126.0).margin(0.05));
    INFO("first beat " << analysis.grid->firstBeatSeconds);
    CHECK(phaseError(analysis.grid->firstBeatSeconds, 0.4, 60.0 / 126.0) < 0.008);
}

TEST_CASE("No grid or key for silence or very short audio")
{
    const auto silent = analyseTrack(*makeTrack(44100.0, 30.0));
    CHECK_FALSE(silent.grid);
    CHECK_FALSE(silent.key);
    CHECK_FALSE(analyseTrack(*makeBeat(124.0, 0.0, 3.0, false)).grid);
}

TEST_CASE("Key is found from the notes played")
{
    // A minor: A in the bass and on top, with C and E.
    const auto am = analyseTrack(*makeChord({{110.0, 1.0}, {220.0, 1.0}, {261.63, 0.6}, {329.63, 0.6}, {440.0, 0.8}}, 10.0));
    REQUIRE(am.key);
    CHECK(am.key->name() == "Am");

    // G major: G, B, D.
    const auto g = analyseTrack(*makeChord({{196.0, 1.0}, {246.94, 0.6}, {293.66, 0.6}, {392.0, 0.8}}, 10.0));
    REQUIRE(g.key);
    CHECK(g.key->name() == "G");
}

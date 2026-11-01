#include "TestSignals.h"

#include "core/Waveform.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace vmp;
using namespace vmp::test;
using Catch::Approx;

TEST_CASE("Each band shows its own part of the spectrum")
{
    struct Case
    {
        double hz;
        int band; // 0 low, 1 mid, 2 high
    };
    for (const auto& c : {Case{60.0, 0}, Case{800.0, 1}, Case{9000.0, 2}})
    {
        INFO(c.hz << " Hz");
        const WaveformPeaks peaks(*makeSine(c.hz, 44100.0, 2.0, 0.8));
        const auto bands = peaks.range(44100, 88200);
        const float levels[] = {bands.low, bands.mid, bands.high};
        CHECK(levels[c.band] == Approx(0.8f).margin(0.08f));
        for (int other = 0; other < 3; ++other)
            if (other != c.band)
                CHECK(levels[other] < 0.3f);
    }
}

TEST_CASE("Peaks are found at every resolution")
{
    auto audio = makeTrack(44100.0, 30.0);
    const int64_t clickAt = 600000;
    for (int64_t f = clickAt; f < clickAt + 200; ++f) // a short high burst
        audio->interleaved[static_cast<size_t>(f) * 2] = audio->interleaved[static_cast<size_t>(f) * 2 + 1] =
            (f % 2 == 0) ? 0.9f : -0.9f;
    const WaveformPeaks peaks(*audio);

    CHECK(peaks.range(clickAt, clickAt + 8).high > 0.5f);               // finest bin
    CHECK(peaks.range(clickAt - 500, clickAt + 500).high > 0.5f);       // 128-frame bins
    CHECK(peaks.range(0, audio->frames).high > 0.5f);                   // coarsest
    CHECK(peaks.range(0, 400000).high == 0.0f);                         // nothing there
}

TEST_CASE("Out-of-range requests are clamped")
{
    const WaveformPeaks peaks(*makeSine(1000.0, 44100.0, 1.0));
    CHECK(peaks.range(-500, 200).mid > 0.0f);
    CHECK(peaks.range(2000000, 3000000).mid == 0.0f);
    CHECK(peaks.range(500, 400).mid == 0.0f);
}

#include "core/Waveform.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace vmp;
using Catch::Approx;

namespace
{
// Silence with a few known peaks.
std::shared_ptr<TrackAudio> makeAudio()
{
    auto audio = std::make_shared<TrackAudio>();
    audio->frames = 1000000;
    audio->interleaved.assign(static_cast<size_t>(audio->frames) * 2, 0.0f);
    audio->interleaved[100 * 2] = 0.9f;          // left, frame 100
    audio->interleaved[500000 * 2 + 1] = -0.7f;  // right, frame 500000
    audio->interleaved[999999 * 2] = 0.3f;       // last frame
    return audio;
}
} // namespace

TEST_CASE("Peaks are found at every resolution")
{
    const WaveformPeaks peaks(makeAudio());

    CHECK(peaks.range(0, 1000000) == std::pair{-0.7f, 0.9f});
    CHECK(peaks.range(99, 101).second == 0.9f);                 // raw samples
    CHECK(peaks.range(0, 1000).second == 0.9f);                 // finest level
    CHECK(peaks.range(400000, 600000).first == -0.7f);          // coarse level
    CHECK(peaks.range(999990, 1000000).second == 0.3f);         // partial last bin
    CHECK(peaks.range(200000, 300000) == std::pair{0.0f, 0.0f});
}

TEST_CASE("Out-of-range requests are clamped")
{
    const WaveformPeaks peaks(makeAudio());
    CHECK(peaks.range(-500, 200).second == 0.9f);
    CHECK(peaks.range(2000000, 3000000) == std::pair{0.0f, 0.0f});
    CHECK(peaks.range(500, 400) == std::pair{0.0f, 0.0f});
}

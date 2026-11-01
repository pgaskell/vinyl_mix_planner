#include "TestSignals.h"

#include "engine/Deck.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace vmp;
using namespace vmp::test;
using Catch::Approx;

namespace
{
constexpr int kBlock = 512;

// Plays a track at a constant speed from `position` (source frames), returning the left channel.
std::vector<float> play(Deck& deck, const TrackAudio& track, double& position, double speed, double deviceRate,
                        int frames)
{
    std::vector<float> left(static_cast<size_t>(frames)), right(static_cast<size_t>(frames));
    const double framesPerOutput = speed * track.sampleRate / deviceRate;
    for (int done = 0; done < frames; done += kBlock)
    {
        const int n = std::min(kBlock, frames - done);
        const double end = position + n * framesPerOutput;
        deck.render(&track, position, end, left.data() + done, right.data() + done, n);
        position = end;
    }
    return left;
}

double frequencyAt(double speed, double fileRate, double deviceRate)
{
    Deck deck;
    deck.prepare(deviceRate, kBlock);
    const auto track = makeSine(1000.0, fileRate, 4.0);
    double position = 0.0;
    play(deck, *track, position, speed, deviceRate, static_cast<int>(0.1 * deviceRate)); // settle
    return measureFrequency(play(deck, *track, position, speed, deviceRate, static_cast<int>(deviceRate)),
                            deviceRate);
}
} // namespace

TEST_CASE("Deck speed changes pitch like varispeed")
{
    CHECK(frequencyAt(1.0, 48000.0, 48000.0) == Approx(1000.0).epsilon(1e-3));
    CHECK(frequencyAt(1.08, 48000.0, 48000.0) == Approx(1080.0).epsilon(1e-3));
    CHECK(frequencyAt(0.92, 48000.0, 48000.0) == Approx(920.0).epsilon(1e-3));
    CHECK(frequencyAt(1.0005, 48000.0, 48000.0) == Approx(1000.5).epsilon(1e-4)); // smallest step
}

TEST_CASE("File and device sample rates may differ")
{
    CHECK(frequencyAt(1.0, 44100.0, 48000.0) == Approx(1000.0).epsilon(1e-3));
    CHECK(frequencyAt(1.04, 44100.0, 96000.0) == Approx(1040.0).epsilon(1e-3));
}

TEST_CASE("A jump or a different track restarts playback at the requested frame")
{
    Deck deck;
    deck.prepare(48000.0, kBlock);
    const auto low = makeSine(500.0, 48000.0, 4.0);
    const auto high = makeSine(2000.0, 48000.0, 4.0);

    double position = 0.0;
    play(deck, *low, position, 1.0, 48000.0, 4800);

    position = 48000.0; // another track, mid-way
    play(deck, *high, position, 1.0, 48000.0, 1024);
    CHECK(measureFrequency(play(deck, *high, position, 1.0, 48000.0, 9600), 48000.0) == Approx(2000.0).epsilon(1e-3));

    position = 10.0; // jump back on the same track
    play(deck, *high, position, 1.0, 48000.0, 1024);
    CHECK(measureFrequency(play(deck, *high, position, 1.0, 48000.0, 9600), 48000.0) == Approx(2000.0).epsilon(1e-3));
}

TEST_CASE("Playing past the end of the track gives silence")
{
    Deck deck;
    deck.prepare(48000.0, kBlock);
    const auto track = makeSine(1000.0, 48000.0, 0.5);
    double position = 0.0;
    play(deck, *track, position, 1.0, 48000.0, 24000 + 2048);
    CHECK(peak(play(deck, *track, position, 1.0, 48000.0, 4800)) == 0.0f);
}

TEST_CASE("A small slip is reached smoothly, a large one jumps")
{
    // A click 1 s into the record.
    auto track = makeTrack(48000.0, 4.0);
    track->interleaved[48000 * 2] = track->interleaved[48000 * 2 + 1] = 1.0f;

    auto clickHeardAt = [&](double slipSeconds) {
        Deck deck;
        deck.prepare(48000.0, kBlock);
        std::vector<float> left(96000), right(96000);
        double position = 0.0;
        for (int done = 0; done < 96000; done += kBlock)
        {
            if (done == 47 * kBlock) // after about 0.5 s, ask for the record to be further on
                position += slipSeconds * 48000.0;
            deck.render(track.get(), position, position + kBlock, left.data() + done, right.data() + done, kBlock);
            position += kBlock;
        }
        const auto loudest = std::max_element(left.begin(), left.end(),
                                              [](float a, float b) { return std::abs(a) < std::abs(b); });
        return static_cast<double>(loudest - left.begin()) / 48000.0;
    };

    CHECK(clickHeardAt(0.0) == Approx(1.0).margin(0.002));
    CHECK(clickHeardAt(0.010) == Approx(0.990).margin(0.002)); // slewed: caught up within ~0.1 s
    CHECK(clickHeardAt(-0.010) == Approx(1.010).margin(0.002));
    CHECK(clickHeardAt(0.300) == Approx(0.700).margin(0.002)); // jumped
}

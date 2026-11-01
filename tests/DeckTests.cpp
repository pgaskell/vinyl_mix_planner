#include "TestSignals.h"

#include "engine/Deck.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace vmp;
using namespace vmp::test;
using Catch::Approx;

namespace
{
// Deck with an instant motor, running a 1 kHz tone.
void setUpRunningDeck(Deck& deck, double fileRate, double deviceRate, int pitchSteps)
{
    deck.prepare(deviceRate, 512);
    deck.setMotorTimes(0.0, 0.0);
    deck.setTrack(makeSine(1000.0, fileRate, 4.0));
    deck.setPitchSteps(pitchSteps);
    deck.setRunning(true);
}

double measuredFrequency(Deck& deck, double deviceRate)
{
    renderDeck(deck, static_cast<int>(0.1 * deviceRate)); // let the resampler settle
    return measureFrequency(renderDeck(deck, static_cast<int>(deviceRate)), deviceRate);
}
} // namespace

TEST_CASE("Deck at 0% plays at the source pitch")
{
    Deck deck;
    setUpRunningDeck(deck, 48000.0, 48000.0, 0);
    CHECK(measuredFrequency(deck, 48000.0) == Approx(1000.0).epsilon(1e-3));
}

TEST_CASE("Pitch fader changes frequency like varispeed")
{
    Deck deck;
    SECTION("+8%")
    {
        setUpRunningDeck(deck, 48000.0, 48000.0, 160);
        CHECK(measuredFrequency(deck, 48000.0) == Approx(1080.0).epsilon(1e-3));
    }
    SECTION("-8%")
    {
        setUpRunningDeck(deck, 48000.0, 48000.0, -160);
        CHECK(measuredFrequency(deck, 48000.0) == Approx(920.0).epsilon(1e-3));
    }
    SECTION("+0.05%, the smallest step")
    {
        setUpRunningDeck(deck, 48000.0, 48000.0, 1);
        CHECK(measuredFrequency(deck, 48000.0) == Approx(1000.5).epsilon(1e-4));
    }
}

TEST_CASE("File and device sample rates may differ")
{
    Deck deck;
    SECTION("44.1 kHz file on a 48 kHz device")
    {
        setUpRunningDeck(deck, 44100.0, 48000.0, 0);
        CHECK(measuredFrequency(deck, 48000.0) == Approx(1000.0).epsilon(1e-3));
    }
    SECTION("44.1 kHz file on a 96 kHz device at +4%")
    {
        setUpRunningDeck(deck, 44100.0, 96000.0, 80);
        CHECK(measuredFrequency(deck, 96000.0) == Approx(1040.0).epsilon(1e-3));
    }
}

TEST_CASE("Position advances in source frames at the pitched speed")
{
    Deck deck;
    setUpRunningDeck(deck, 44100.0, 48000.0, 160);
    renderDeck(deck, 48000);
    // One second at +8% consumes 1.08 s of source, give or take resampler look-ahead.
    CHECK(static_cast<double>(deck.positionFrames()) == Approx(1.08 * 44100.0).margin(200.0));
}

TEST_CASE("A stopped deck is silent and does not advance")
{
    Deck deck;
    setUpRunningDeck(deck, 48000.0, 48000.0, 0);
    deck.setRunning(false);
    const auto out = renderDeck(deck, 4800);
    CHECK(deck.positionFrames() == 0);
    for (float s : out)
        REQUIRE(s == 0.0f);
}

TEST_CASE("Motor start is gradual")
{
    Deck deck;
    deck.prepare(48000.0, 512);
    deck.setMotorTimes(0.7, 0.5);
    deck.setTrack(makeSine(1000.0, 48000.0, 4.0));
    deck.setRunning(true);
    renderDeck(deck, 16800); // 0.35 s: half way up, so ~0.0875 s of source consumed
    CHECK(static_cast<double>(deck.positionFrames()) == Approx(0.0875 * 48000.0).margin(600.0));
}

TEST_CASE("Seek moves the needle")
{
    Deck deck;
    setUpRunningDeck(deck, 48000.0, 48000.0, 0);
    deck.seek(96000);
    renderDeck(deck, 512);
    CHECK(deck.positionFrames() >= 96000);
    CHECK(deck.positionFrames() < 96000 + 2048);
}

TEST_CASE("Running off the end of the record gives silence")
{
    Deck deck;
    deck.prepare(48000.0, 512);
    deck.setMotorTimes(0.0, 0.0);
    deck.setTrack(makeSine(1000.0, 48000.0, 0.5));
    deck.setRunning(true);
    renderDeck(deck, 48000);
    const auto tail = renderDeck(deck, 4800);
    for (float s : tail)
        REQUIRE(s == 0.0f);
}

TEST_CASE("Loading a new track restarts from the top and frees the old one")
{
    Deck deck;
    setUpRunningDeck(deck, 48000.0, 48000.0, 0);
    renderDeck(deck, 48000);
    REQUIRE(deck.positionFrames() > 40000);

    deck.setTrack(makeSine(500.0, 48000.0, 4.0));
    renderDeck(deck, 512);
    CHECK(deck.positionFrames() < 2048);
    deck.collectGarbage();

    // A further swap must not stall behind the freed retired slot.
    deck.setTrack(makeSine(250.0, 48000.0, 4.0));
    renderDeck(deck, 4800);
    deck.collectGarbage();
    renderDeck(deck, 4800);
    CHECK(measuredFrequency(deck, 48000.0) == Approx(250.0).epsilon(1e-3));
}

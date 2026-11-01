#include "TestSignals.h"

#include "engine/Engine.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace vmp;
using namespace vmp::test;
using Catch::Approx;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 512;

struct Outputs
{
    explicit Outputs(int channels) : data(static_cast<size_t>(channels))
    {
        for (auto& ch : data)
            ch.assign(kBlock, 0.0f);
        for (auto& ch : data)
            pointers.push_back(ch.data());
    }
    int size() const { return static_cast<int>(pointers.size()); }
    float peak(int ch) const
    {
        float p = 0.0f;
        for (float s : data[static_cast<size_t>(ch)])
            p = std::max(p, std::abs(s));
        return p;
    }
    std::vector<std::vector<float>> data;
    std::vector<float*> pointers;
};

void startDeck(Engine& engine, int index, TrackAudio* track)
{
    engine.deck(index).setMotorTimes(0.0, 0.0);
    engine.deck(index).setTrack(track);
    engine.deck(index).setRunning(true);
}

void run(Engine& engine, Outputs& out, int blocks)
{
    for (int i = 0; i < blocks; ++i)
        engine.render(out.pointers.data(), out.size(), kBlock);
}
} // namespace

TEST_CASE("Direct out sends each deck dry to its own pair")
{
    Engine engine;
    engine.prepare(kRate, kBlock, {0, 1, 2, 3});
    engine.setOutputMode(OutputMode::DirectOut);
    engine.setDeckOutput(0, 2);
    engine.setDeckOutput(1, Engine::kNoOutput);
    engine.setChannelGain(0, 0.0f); // the internal fader has no effect in direct mode
    startDeck(engine, 0, makeSine(1000.0, kRate, 2.0));
    startDeck(engine, 1, makeSine(1000.0, kRate, 2.0));

    Outputs out(4);
    run(engine, out, 20);
    CHECK(out.peak(0) == 0.0f);
    CHECK(out.peak(1) == 0.0f);
    CHECK(out.peak(2) > 0.4f);
    CHECK(out.peak(3) > 0.4f);
}

TEST_CASE("Routing honours disabled device channels")
{
    // Physical outputs 3/4 disabled, so physical 5/6 arrive as buffers 2/3.
    Engine engine;
    engine.prepare(kRate, kBlock, {0, 1, -1, -1, 2, 3});
    engine.setOutputMode(OutputMode::DirectOut);
    engine.setDeckOutput(0, 4);
    engine.setDeckOutput(1, 2); // disabled: dropped, not misrouted
    startDeck(engine, 0, makeSine(1000.0, kRate, 2.0));
    startDeck(engine, 1, makeSine(1000.0, kRate, 2.0));

    Outputs out(4);
    run(engine, out, 20);
    CHECK(out.peak(0) == 0.0f);
    CHECK(out.peak(2) > 0.4f);
}

TEST_CASE("Outputs beyond the device are ignored")
{
    Engine engine;
    engine.prepare(kRate, kBlock, {0, 1});
    engine.setOutputMode(OutputMode::DirectOut);
    engine.setDeckOutput(0, 6);
    startDeck(engine, 0, makeSine(1000.0, kRate, 2.0));

    Outputs out(2);
    run(engine, out, 4);
    CHECK(out.peak(0) == 0.0f);
}

TEST_CASE("Internal mix sums decks through their channel gains")
{
    Engine engine;
    engine.prepare(kRate, kBlock, {0, 1, 2, 3});
    engine.setOutputMode(OutputMode::InternalMix);
    engine.setMasterOutput(0);
    engine.setChannelGain(0, 1.0f);
    engine.setChannelGain(1, 0.5f);
    startDeck(engine, 0, makeConstant(0.25f, kRate, 2.0));
    startDeck(engine, 1, makeConstant(0.25f, kRate, 2.0));

    Outputs out(4);
    run(engine, out, 20);
    CHECK(out.data[0].back() == Approx(0.375f).margin(1e-3));
    CHECK(out.data[1].back() == Approx(0.375f).margin(1e-3));
    CHECK(out.peak(2) == 0.0f); // deck pairs are unused in internal mode
}

TEST_CASE("Only the active deck count is rendered")
{
    Engine engine;
    engine.prepare(kRate, kBlock, {0, 1, 2, 3, 4, 5, 6, 7});
    engine.setOutputMode(OutputMode::DirectOut);
    engine.setNumDecks(2);
    startDeck(engine, 2, makeSine(1000.0, kRate, 2.0));

    Outputs out(8);
    run(engine, out, 4);
    CHECK(out.peak(4) == 0.0f);

    engine.setNumDecks(3);
    run(engine, out, 4);
    CHECK(out.peak(4) > 0.4f);
}

TEST_CASE("Blocks larger than announced are rendered in chunks")
{
    Engine engine;
    engine.prepare(kRate, 128, {0, 1});
    engine.setOutputMode(OutputMode::InternalMix);
    startDeck(engine, 0, makeConstant(0.25f, kRate, 2.0));

    std::vector<float> left(3000), right(3000);
    float* ptrs[] = {left.data(), right.data()};
    for (int i = 0; i < 10; ++i)
        engine.render(ptrs, 2, 3000);
    CHECK(left.back() == Approx(0.25f).margin(1e-3));
    CHECK(left[1500] == Approx(0.25f).margin(1e-3));
}

#include "TestSignals.h"

#include "core/Edits.h"
#include "engine/Engine.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <map>

using namespace vmp;
using namespace vmp::test;
using Catch::Approx;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 512;

// A set plus the decoded audio for each of its tracks.
struct Fixture
{
    Set set;
    std::map<std::string, std::shared_ptr<TrackAudio>> audio;
    Engine engine;

    Fixture(int outputs = 4)
    {
        std::vector<int> map;
        for (int i = 0; i < outputs; ++i)
            map.push_back(i);
        engine.prepare(kRate, kBlock, map);
        engine.setMasterOutput(0);
    }

    Id addClip(std::shared_ptr<TrackAudio> track, int deck, double start, int pitch = 0)
    {
        const std::string path = "/track" + std::to_string(audio.size());
        audio[path] = track;
        const Id trackId =
            edit::addTrack(set, {0, path, path, track->frames, track->sampleRate, PressedSpeed::Unknown, {}, {}});
        const Id clipId = edit::addClip(set, trackId, deck, start);
        edit::setClipPitch(set, clipId, pitch);
        return clipId;
    }

    void publish()
    {
        engine.setSchedule(buildSchedule(set, [this](const Track& t) -> std::shared_ptr<const TrackAudio> {
            auto it = audio.find(t.path);
            return it == audio.end() ? nullptr : it->second;
        }));
    }

    // Renders `seconds` of output and returns each output channel.
    std::vector<std::vector<float>> run(double seconds, int outputs = 4)
    {
        const auto frames = static_cast<size_t>(seconds * kRate);
        std::vector<std::vector<float>> out(static_cast<size_t>(outputs), std::vector<float>(frames));
        std::vector<float*> block(static_cast<size_t>(outputs));
        for (size_t done = 0; done < frames; done += kBlock)
        {
            const int n = static_cast<int>(std::min<size_t>(kBlock, frames - done));
            for (size_t ch = 0; ch < out.size(); ++ch)
                block[ch] = out[ch].data() + done;
            engine.render(block.data(), outputs, n);
        }
        return out;
    }
};
} // namespace

TEST_CASE("A clip starts at its timeline position and plays at its pitch")
{
    Fixture f;
    f.addClip(makeSine(1000.0, kRate, 10.0), 0, 1.0, 160);
    f.publish();
    f.engine.play();

    const auto out = f.run(3.0);
    CHECK(peak(out[0], 0, static_cast<size_t>(0.99 * kRate)) == 0.0f);
    CHECK(measureFrequency(slice(out[0], 1.5, 2.5, kRate), kRate) == Approx(1080.0).epsilon(1e-3));
}

TEST_CASE("Playback lands on the timeline where the clip timing says")
{
    // A click 2 s into a 44.1 kHz track, needle drop at 0.5 s, +8%: heard at 0.5 + 2 / 1.08 s.
    Fixture f;
    auto track = makeTrack(44100.0, 5.0);
    track->interleaved[88200 * 2] = track->interleaved[88200 * 2 + 1] = 1.0f;
    f.addClip(track, 0, 0.5, 160);
    f.publish();
    f.engine.play();

    const auto out = f.run(3.0);
    const auto loudest = std::max_element(out[0].begin(), out[0].end(),
                                          [](float a, float b) { return std::abs(a) < std::abs(b); });
    const double heardAt = static_cast<double>(loudest - out[0].begin()) / kRate;
    CHECK(heardAt == Approx(0.5 + 2.0 / 1.08).margin(0.002)); // within the resampler's latency
}

TEST_CASE("Clips on one deck play in turn, with silence between")
{
    Fixture f;
    const Id first = f.addClip(makeSine(500.0, kRate, 10.0), 0, 0.0);
    edit::trimClipEnd(f.set, first, 2.0);
    f.addClip(makeSine(2000.0, kRate, 10.0), 0, 2.5);
    f.publish();
    f.engine.play();

    const auto out = f.run(4.0);
    CHECK(measureFrequency(slice(out[0], 1.0, 1.9, kRate), kRate) == Approx(500.0).epsilon(1e-3));
    CHECK(peak(slice(out[0], 2.05, 2.45, kRate)) < 1e-4f); // the EQ filters ring out briefly
    CHECK(measureFrequency(slice(out[0], 3.0, 3.9, kRate), kRate) == Approx(2000.0).epsilon(1e-3));
}

TEST_CASE("Internal mix applies each clip's fader and EQ")
{
    Fixture f;
    const Id a = f.addClip(makeSine(1000.0, kRate, 10.0), 0, 0.0);
    const Id b = f.addClip(makeSine(80.0, kRate, 10.0), 1, 0.0);
    f.engine.setOutputMode(OutputMode::InternalMix);

    SECTION("centred controls pass both")
    {
        f.publish();
        f.engine.play();
        CHECK(peak(slice(f.run(2.0)[0], 1.0, 2.0, kRate)) == Approx(1.0f).epsilon(0.05));
    }
    SECTION("fader down and low kill silence the mix")
    {
        edit::setClipMixer(f.set, a, {0.0, 0.5, 0.5, 0.5});
        edit::setClipMixer(f.set, b, {1.0, 0.5, 0.5, 0.0});
        f.publish();
        f.engine.play();
        CHECK(peak(slice(f.run(2.0)[0], 1.0, 2.0, kRate)) < 0.01f);
    }
}

TEST_CASE("Direct out sends each deck dry to its own pair")
{
    Fixture f;
    const Id a = f.addClip(makeSine(1000.0, kRate, 10.0), 0, 0.0);
    f.addClip(makeSine(1000.0, kRate, 10.0), 1, 0.0);
    edit::setClipMixer(f.set, a, {0.0, 0.0, 0.0, 0.0}); // ignored: the hardware mixes
    f.engine.setOutputMode(OutputMode::DirectOut);
    f.engine.setDeckOutput(0, 2);
    f.engine.setDeckOutput(1, Engine::kNoOutput);
    f.publish();
    f.engine.play();

    const auto out = f.run(0.5);
    CHECK(peak(out[0]) == 0.0f);
    CHECK(peak(out[2]) > 0.45f);
    CHECK(peak(out[3]) > 0.45f);
}

TEST_CASE("Routing honours disabled device channels")
{
    // Physical outputs 3/4 disabled, so physical 5/6 arrive as buffers 2/3.
    Fixture f;
    f.engine.prepare(kRate, kBlock, {0, 1, -1, -1, 2, 3});
    f.addClip(makeSine(1000.0, kRate, 10.0), 0, 0.0);
    f.addClip(makeSine(1000.0, kRate, 10.0), 1, 0.0);
    f.engine.setOutputMode(OutputMode::DirectOut);
    f.engine.setDeckOutput(0, 4);
    f.engine.setDeckOutput(1, 2); // disabled: dropped, not misrouted
    f.publish();
    f.engine.play();

    const auto out = f.run(0.5);
    CHECK(peak(out[0]) == 0.0f);
    CHECK(peak(out[2]) > 0.45f);
}

TEST_CASE("Pause fades out and stops the playhead; seek moves it")
{
    Fixture f;
    f.addClip(makeConstant(0.25f, kRate, 10.0), 0, 0.0);
    f.publish();
    f.engine.play();
    f.run(1.0);
    CHECK(f.engine.playheadSeconds() == Approx(1.0).margin(kBlock / kRate));

    f.engine.pause();
    const auto fade = f.run(kBlock / kRate);
    CHECK(std::abs(fade[0].back()) < 0.01f);
    const double stoppedAt = f.engine.playheadSeconds();
    CHECK(peak(f.run(0.5)[0]) == 0.0f);
    CHECK(f.engine.playheadSeconds() == stoppedAt);

    f.engine.seek(5.0);
    f.run(0.01);
    CHECK(f.engine.playheadSeconds() == 5.0);
}

TEST_CASE("Clips without loaded audio are left out of the schedule")
{
    Fixture f;
    f.addClip(makeSine(1000.0, kRate, 10.0), 0, 0.0);
    f.audio.clear();
    f.publish();
    f.engine.play();
    CHECK(peak(f.run(0.5)[0]) == 0.0f);
}

TEST_CASE("Blocks larger than announced are rendered in chunks")
{
    Fixture f;
    f.engine.prepare(kRate, 128, {0, 1});
    f.addClip(makeConstant(0.25f, kRate, 10.0), 0, 0.0);
    f.publish();
    f.engine.play();

    std::vector<float> left(3000), right(3000);
    float* ptrs[] = {left.data(), right.data()};
    for (int i = 0; i < 10; ++i)
        f.engine.render(ptrs, 2, 3000);
    CHECK(left[1500] == Approx(0.25f).margin(1e-3));
    CHECK(left.back() == Approx(0.25f).margin(1e-3));
}

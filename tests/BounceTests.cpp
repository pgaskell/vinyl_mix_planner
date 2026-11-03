#include "TestSignals.h"

#include "core/Edits.h"
#include "core/StreamedAudio.h"
#include "engine/Bounce.h"
#include "engine/Engine.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include <unistd.h>

using namespace vmp;
using namespace vmp::test;
using Catch::Approx;

namespace
{
constexpr double kRate = 48000.0;

struct TempDir
{
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("vmp-bounce-" + std::to_string(::getpid()) + "-" + std::to_string(next++));
    TempDir() { std::filesystem::create_directories(path); }
    ~TempDir() { std::filesystem::remove_all(path); }
    static inline int next = 0;
};

std::shared_ptr<TrackAudio> streamed(const TrackAudio& audio, const std::filesystem::path& file)
{
    REQUIRE(StreamedAudio::write(file, audio));
    auto track = std::make_shared<TrackAudio>();
    track->frames = audio.frames;
    track->sampleRate = audio.sampleRate;
    track->stream = StreamedAudio::open(file);
    REQUIRE(track->stream != nullptr);
    return track;
}

// One clip on deck 1 of a set, with the audio it plays.
struct OneClip
{
    Set set;
    std::shared_ptr<const TrackAudio> audio;
    Id clip = 0;

    OneClip(std::shared_ptr<const TrackAudio> track, double start) : audio(std::move(track))
    {
        const Id trackId =
            edit::addTrack(set, {0, "/a", "a", audio->frames, audio->sampleRate, PressedSpeed::Unknown, {}, {}});
        clip = edit::addClip(set, trackId, 0, start);
    }

    std::unique_ptr<Schedule> schedule() const
    {
        return buildSchedule(set, [this](const Track&) { return audio; });
    }
};

struct Bounced
{
    BounceResult result;
    std::vector<float> left, right;
};

Bounced run(const Schedule& schedule, BounceRange range, std::function<bool(double)> progress = {})
{
    Bounced out;
    out.result = bounce(
        schedule, kRate, range,
        [&](const float* const* channels, int n) {
            out.left.insert(out.left.end(), channels[0], channels[0] + n);
            out.right.insert(out.right.end(), channels[1], channels[1] + n);
            return true;
        },
        std::move(progress));
    return out;
}
} // namespace

TEST_CASE("The bounce covers the set from the first needle drop to the last clip's end")
{
    OneClip one(makeSine(1000.0, kRate, 3.0), 2.0);
    const auto range = bounceRange(*one.schedule());
    REQUIRE(range);
    CHECK(range->start == Approx(2.0));
    CHECK(range->end == Approx(5.0));
    CHECK_FALSE(bounceRange(Schedule{}));
}

TEST_CASE("A bounce of streamed audio matches live playback, and leaves the live streams alone")
{
    TempDir dir;
    const auto memory = makeSine(1000.0, 44100.0, 6.0);
    OneClip live(memory, 1.0);
    OneClip disk(streamed(*memory, dir.path / "a.f32"), 1.0);
    for (auto* s : {&live, &disk})
        edit::setClipPitch(s->set, s->clip, 117);

    // Live playback, from memory, seeking to where the bounce starts.
    Engine engine;
    engine.prepare(kRate, 512, {0, 1});
    engine.setMasterOutput(0);
    engine.setClock([]() -> int64_t { return 0; });
    engine.setSchedule(live.schedule());
    const auto range = *bounceRange(*disk.schedule());
    engine.seek(range.start);
    engine.playWithoutFade();
    std::vector<float> expected(static_cast<size_t>(4.0 * kRate));
    for (size_t done = 0; done < expected.size(); done += 512)
    {
        std::vector<float> right(512);
        float* block[] = {expected.data() + done, right.data()};
        engine.render(block, 2, static_cast<int>(std::min<size_t>(512, expected.size() - done)));
    }

    const auto bounced = run(*disk.schedule(), range);
    CHECK(bounced.result.status == BounceResult::Status::Done);
    CHECK(bounced.result.glitches.empty());
    CHECK(bounced.result.frames == std::llround((range.end - range.start) * kRate));
    CHECK(bounced.left.size() == static_cast<size_t>(bounced.result.frames));
    float worst = 0.0f;
    for (size_t i = 0; i < expected.size(); ++i)
        worst = std::max(worst, std::abs(expected[i] - bounced.left[i]));
    CHECK(worst < 1e-4f); // the same up to rounding: live renders in smaller blocks
    CHECK(std::abs(bounced.left[1]) > 0.01f); // no fade-in at the start
    // The largest sample written (here the resampler's ringing at the needle drop, a little over the
    // sine's 0.5).
    float largest = 0.0f;
    for (const auto* channel : {&bounced.left, &bounced.right})
        for (float x : *channel)
            largest = std::max(largest, std::abs(x));
    CHECK(bounced.result.peak == largest);
    CHECK(bounced.result.clippedSamples == 0);

    // The bounce loaded its own copy of the file: nothing is left loaded in the live stream.
    int64_t count = 0;
    CHECK(disk.audio->framesAt(0, count) == nullptr);
}

TEST_CASE("A bounce reports samples past full scale")
{
    OneClip one(makeSine(1000.0, kRate, 2.0, 0.9), 0.0);
    edit::setClipTrim(one.set, one.clip, 1.0); // +9 dB
    const auto bounced = run(*one.schedule(), *bounceRange(*one.schedule()));
    CHECK(bounced.result.status == BounceResult::Status::Done);
    CHECK(bounced.result.peak > 2.0f);
    CHECK(bounced.result.clippedSamples > 0);
}

TEST_CASE("A bounce refuses to start while a clip has no audio, and can be cancelled")
{
    OneClip one(makeSine(1000.0, kRate, 2.0), 0.0);
    const auto missing = buildSchedule(one.set, [](const Track&) { return nullptr; });
    CHECK(run(*missing, {0.0, 2.0}).result.status == BounceResult::Status::MissingAudio);

    int calls = 0;
    const auto cancelled = run(*one.schedule(), {0.0, 2.0}, [&](double) { return ++calls < 3; });
    CHECK(cancelled.result.status == BounceResult::Status::Cancelled);
    CHECK(cancelled.left.size() < static_cast<size_t>(2.0 * kRate));
}

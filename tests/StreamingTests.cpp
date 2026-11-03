#include "TestSignals.h"

#include "core/Edits.h"
#include "core/StreamedAudio.h"
#include "engine/Engine.h"
#include "engine/MixerChannel.h"
#include "engine/Prefetcher.h"

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
// What the internal mix does to a record with every control at its default: the trim's headroom.
const float kDefaultGain = MixerChannel::trimGain(0.5);
constexpr int kBlock = 512;

// One per test: ctest runs tests in parallel processes.
struct TempDir
{
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("vmp-streaming-" + std::to_string(::getpid()) + "-" + std::to_string(next++));
    TempDir() { std::filesystem::create_directories(path); }
    ~TempDir() { std::filesystem::remove_all(path); }
    static inline int next = 0;
};

// The same audio, streamed from a cache file.
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

struct Fixture
{
    Set set;
    std::shared_ptr<TrackAudio> audio;
    Engine engine;
    Id clip = 0;

    Fixture(std::shared_ptr<TrackAudio> track, double start = 0.0) : audio(std::move(track))
    {
        engine.prepare(kRate, kBlock, {0, 1});
        engine.setMasterOutput(0);
        engine.setClock([]() -> int64_t { return 0; }); // a slow test machine isn't an overload
        const Id trackId =
            edit::addTrack(set, {0, "/a", "a", audio->frames, audio->sampleRate, PressedSpeed::Unknown, {}, {}});
        clip = edit::addClip(set, trackId, 0, start);
    }

    std::unique_ptr<Schedule> schedule()
    {
        return buildSchedule(set, [this](const Track&) -> std::shared_ptr<const TrackAudio> { return audio; });
    }

    std::vector<float> run(double seconds, Prefetcher* prefetcher = nullptr)
    {
        const auto frames = static_cast<size_t>(seconds * kRate);
        std::vector<float> left(frames), right(frames);
        for (size_t done = 0; done < frames; done += kBlock)
        {
            if (prefetcher != nullptr)
                prefetcher->update();
            const int n = static_cast<int>(std::min<size_t>(kBlock, frames - done));
            float* block[] = {left.data() + done, right.data() + done};
            engine.render(block, 2, n);
        }
        return left;
    }

    std::vector<Glitch> glitches()
    {
        std::vector<Glitch> all;
        Glitch g;
        while (engine.popGlitch(g))
            all.push_back(g);
        return all;
    }
};
} // namespace

TEST_CASE("Cache files read back exactly, and damaged ones are refused")
{
    TempDir dir;
    const auto audio = makeSine(440.0, 44100.0, 2.5);
    const auto file = dir.path / "a.f32";
    REQUIRE(StreamedAudio::write(file, *audio));

    auto stream = StreamedAudio::open(file);
    REQUIRE(stream);
    CHECK(stream->frames() == audio->frames);
    CHECK(stream->sampleRate() == 44100.0);
    CHECK(stream->numChunks() == (audio->frames + StreamedAudio::kChunkFrames - 1) / StreamedAudio::kChunkFrames);
    CHECK(stream->readAll()->interleaved == audio->interleaved);

    std::vector<float> chunk(StreamedAudio::kChunkFloats);
    const int64_t last = stream->numChunks() - 1;
    REQUIRE(stream->read(last, chunk.data()));
    const auto offset = static_cast<size_t>(last * StreamedAudio::kChunkFrames) * 2;
    CHECK(chunk[0] == audio->interleaved[offset]);
    CHECK(chunk[static_cast<size_t>(stream->chunkFrames(last)) * 2 - 1] == audio->interleaved.back());

    // Nothing is loaded until someone loads it.
    CHECK(stream->chunk(0) == nullptr);

    CHECK_FALSE(StreamedAudio::open(dir.path / "missing.f32"));
    std::filesystem::resize_file(file, std::filesystem::file_size(file) - 8);
    CHECK_FALSE(StreamedAudio::open(file));
    for (const auto& entry : std::filesystem::directory_iterator(dir.path))
        CHECK(entry.path().extension() != ".tmp");
}

TEST_CASE("Reading a chunk of a file truncated after opening fails instead of crashing")
{
    TempDir dir;
    const auto file = dir.path / "a.f32";
    REQUIRE(StreamedAudio::write(file, *makeSine(440.0, 44100.0, 3.0)));
    auto stream = StreamedAudio::open(file);
    REQUIRE(stream);
    std::filesystem::resize_file(file, 20000);
    std::vector<float> chunk(StreamedAudio::kChunkFloats);
    CHECK_FALSE(stream->read(2, chunk.data()));
}

TEST_CASE("Streamed audio plays exactly like audio in memory, across chunk boundaries and pitch")
{
    TempDir dir;
    const auto memory = makeSine(1000.0, 44100.0, 6.0);
    Fixture inMemory(memory, 0.5);
    Fixture fromDisk(streamed(*memory, dir.path / "a.f32"), 0.5);
    for (auto* f : {&inMemory, &fromDisk})
        edit::setClipPitch(f->set, f->clip, 117); // +5.85%: chunk edges land mid-block
    inMemory.engine.setSchedule(inMemory.schedule());

    Prefetcher prefetcher(fromDisk.engine, 64);
    prefetcher.setSchedule(fromDisk.schedule());
    prefetcher.update();
    CHECK(prefetcher.stats().chunksLoaded > 0);
    int64_t count = 0;
    const float* loaded = fromDisk.audio->framesAt(1000, count);
    REQUIRE(loaded != nullptr);
    CHECK(count == StreamedAudio::kChunkFrames - 1000);
    CHECK(loaded[2] == memory->interleaved[2002]);

    inMemory.engine.play();
    fromDisk.engine.play();
    const auto expected = inMemory.run(5.0);
    const auto actual = fromDisk.run(5.0, &prefetcher);
    CHECK(fromDisk.glitches().empty());
    float worst = 0.0f;
    for (size_t i = 0; i < expected.size(); ++i)
        worst = std::max(worst, std::abs(expected[i] - actual[i]));
    CHECK(worst < 1e-6f);
    CHECK(peak(actual, static_cast<size_t>(kRate), static_cast<size_t>(4 * kRate)) > 0.4f * kDefaultGain);
}

TEST_CASE("Starting where nothing is loaded holds the playhead, then plays on and reports underruns")
{
    TempDir dir;
    Fixture f(streamed(*makeSine(1000.0, 44100.0, 10.0), dir.path / "a.f32"));
    f.engine.setSchedule(f.schedule()); // bypassing the prefetcher: nothing will ever load
    f.engine.play();

    f.run(0.25);
    CHECK(f.engine.playheadSeconds() == 0.0);
    CHECK(f.glitches().empty());

    const auto out = f.run(1.0); // past the pre-roll
    CHECK(f.engine.playheadSeconds() > 0.5);
    CHECK(peak(out) == 0.0f);
    const auto glitches = f.glitches();
    REQUIRE(glitches.size() == 1);
    CHECK(glitches[0].kind == GlitchKind::Underrun);
    CHECK(glitches[0].deck == 0);

    f.engine.seek(5.0); // reported again after a seek
    f.run(1.0);
    CHECK(f.glitches().size() == 1);
}

TEST_CASE("Pre-roll waits for the prefetcher, then plays without a glitch")
{
    TempDir dir;
    Fixture f(streamed(*makeSine(1000.0, 44100.0, 10.0), dir.path / "a.f32"));
    Prefetcher prefetcher(f.engine, 64);
    prefetcher.setSchedule(f.schedule());
    f.engine.play();
    f.run(0.1); // the prefetcher hasn't run yet: the engine has no schedule, nothing to wait for
    prefetcher.update();
    f.engine.seek(4.0);
    f.run(0.1, &prefetcher);
    CHECK(f.engine.playheadSeconds() == Approx(4.1).margin(kBlock / kRate));
    CHECK(f.glitches().empty());
}

TEST_CASE("A small pool keeps up with a seek by giving up chunks far from the playhead")
{
    TempDir dir;
    Fixture f(streamed(*makeSine(1000.0, kRate, 60.0), dir.path / "a.f32"));
    Prefetcher prefetcher(f.engine, 24); // ~16 s of audio at 48 kHz
    prefetcher.setSchedule(f.schedule());
    prefetcher.update();
    f.engine.play();
    f.run(1.0, &prefetcher);
    CHECK(prefetcher.stats().chunksLoaded <= 24);

    f.engine.seek(40.0);
    const auto out = f.run(2.0, &prefetcher);
    CHECK(f.glitches().empty());
    CHECK(peak(out, static_cast<size_t>(0.5 * kRate)) > 0.4f * kDefaultGain);
    CHECK(f.engine.playheadSeconds() == Approx(42.0).margin(0.05));
}

TEST_CASE("Moving a playing clip far away doesn't underrun: the schedule waits for its audio")
{
    TempDir dir;
    Fixture f(streamed(*makeSine(1000.0, kRate, 60.0), dir.path / "a.f32"));
    Prefetcher prefetcher(f.engine, 64);
    prefetcher.setSchedule(f.schedule());
    prefetcher.update();
    f.engine.play();
    f.run(1.0, &prefetcher);

    edit::moveClip(f.set, f.clip, -30.0, 0); // the playhead is now 31 s into the track
    prefetcher.setSchedule(f.schedule());
    const auto out = f.run(1.0, &prefetcher);
    CHECK(f.glitches().empty());
    CHECK(peak(out, static_cast<size_t>(0.1 * kRate)) > 0.4f * kDefaultGain);
}

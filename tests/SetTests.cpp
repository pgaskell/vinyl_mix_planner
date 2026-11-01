#include "core/Set.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace vmp;
using Catch::Approx;

namespace
{
constexpr double kRate = 44100.0;

int64_t frames(double seconds)
{
    return static_cast<int64_t>(seconds * kRate);
}

// Two six-minute tracks, ids 1 and 2.
Set makeSet()
{
    Set set;
    set.name = "Test";
    set.tracks.push_back({1, "/music/a.flac", "Track A", frames(360.0), kRate, PressedSpeed::Unknown, {}, {}});
    set.tracks.push_back({2, "/music/b.flac", "Track B", frames(360.0), kRate, PressedSpeed::Rpm33, {}, {}});
    set.nextId = 3;
    return set;
}

Clip& addClip(Set& set, Id track, int deck, double start, double inSeconds, double outSeconds, int pitch = 0)
{
    Clip clip;
    clip.id = set.allocateId();
    clip.trackId = track;
    clip.deck = deck;
    clip.startSeconds = start;
    clip.inFrame = frames(inSeconds);
    clip.outFrame = frames(outSeconds);
    clip.pitchSteps = pitch;
    set.clips.push_back(clip);
    return set.clips.back();
}

size_t count(const std::vector<Issue>& issues, Issue::Severity severity)
{
    return static_cast<size_t>(
        std::count_if(issues.begin(), issues.end(), [=](const Issue& i) { return i.severity == severity; }));
}
} // namespace

TEST_CASE("Clip length on the timeline depends on pitch")
{
    auto set = makeSet();
    const auto& clip = addClip(set, 1, 0, 100.0, 0.0, 60.0, 80); // +4%
    const ClipTiming timing(clip, *set.findTrack(1));
    CHECK(timing.endSeconds() == Approx(100.0 + 60.0 / 1.04));
}

TEST_CASE("Clip timing maps between timeline and source frames")
{
    auto set = makeSet();
    auto& clip = addClip(set, 1, 0, 100.0, 10.0, 200.0);
    // Hold 0% to 40 s of music, then slide to +6% by 46.18 s (6 s at an average of +3%).
    clip.points(Param::Pitch) = {{frames(40.0), 0.0}, {frames(40.0 + 6.0 * 1.03), 120.0}};
    const ClipTiming timing(clip, *set.findTrack(1));

    CHECK(timing.sourceFrameAt(100.0) == Approx(static_cast<double>(frames(10.0))));
    CHECK(timing.sourceFrameAt(101.0) == Approx(static_cast<double>(frames(11.0))));
    CHECK(timing.timelineSecondsAt(static_cast<double>(frames(40.0))) == Approx(130.0));
    for (double t = 100.0; t < 250.0; t += 1.7)
        CHECK(timing.timelineSecondsAt(timing.sourceFrameAt(t)) == Approx(t));
}

TEST_CASE("Trimming the in-point keeps pitch automation on the same part of the music")
{
    auto set = makeSet();
    auto& full = addClip(set, 1, 0, 0.0, 0.0, 300.0);
    full.points(Param::Pitch) = {{frames(120.0), 0.0}, {frames(128.0), 100.0}};
    auto trimmed = full;
    trimmed.inFrame = frames(60.0);
    trimmed.startSeconds = 500.0;

    const auto& track = *set.findTrack(1);
    const ClipTiming a(full, track), b(trimmed, track);
    const double moveA = a.timelineSecondsAt(static_cast<double>(frames(120.0)));
    const double moveB = b.timelineSecondsAt(static_cast<double>(frames(120.0)));
    CHECK(moveA == Approx(120.0));
    CHECK(moveB == Approx(560.0));
    CHECK(b.speedAt(moveB + 4.0) == Approx(a.speedAt(moveA + 4.0)));
}

TEST_CASE("A playable set has no issues")
{
    auto set = makeSet();
    addClip(set, 1, 0, 0.0, 0.0, 300.0);
    addClip(set, 2, 1, 280.0, 0.0, 300.0, -40); // ends at 280 + 300 / 0.98 = 586.1
    addClip(set, 1, 0, 400.0, 30.0, 200.0);     // A again on deck 1 after a 100 s changeover
    CHECK(validate(set).empty());
}

TEST_CASE("The same record on two decks at once needs two copies")
{
    auto set = makeSet();
    addClip(set, 1, 0, 0.0, 0.0, 300.0);
    addClip(set, 1, 1, 200.0, 0.0, 300.0);
    const auto issues = validate(set);
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].severity == Issue::Severity::Warning);
    CHECK(issues[0].message.find("two copies") != std::string::npos);
}

TEST_CASE("Two records can't play on one deck at once")
{
    auto set = makeSet();
    const auto a = addClip(set, 1, 0, 0.0, 0.0, 300.0).id;
    const auto b = addClip(set, 2, 0, 290.0, 0.0, 300.0).id;
    const auto issues = validate(set);
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].severity == Issue::Severity::Error);
    CHECK(issues[0].clipIds == std::vector<Id>{a, b});
    CHECK(issues[0].message.find("overlap by 10.0 s") != std::string::npos);
}

TEST_CASE("Overlap is found against a long clip, not just the previous one")
{
    auto set = makeSet();
    addClip(set, 1, 0, 0.0, 0.0, 300.0);
    addClip(set, 2, 0, 10.0, 0.0, 10.0);
    addClip(set, 2, 0, 40.0, 0.0, 10.0);
    CHECK(count(validate(set), Issue::Severity::Error) == 2);
}

TEST_CASE("A record change needs time")
{
    auto set = makeSet();
    addClip(set, 1, 0, 0.0, 0.0, 100.0);
    addClip(set, 2, 0, 102.0, 0.0, 100.0);
    auto issues = validate(set);
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].severity == Issue::Severity::Warning);

    set.clips[1].startSeconds = 100.0 + kMinChangeoverSeconds;
    CHECK(validate(set).empty());
}

TEST_CASE("Structural problems are errors")
{
    auto set = makeSet();
    SECTION("deck beyond the set's deck count")
    {
        addClip(set, 1, 2, 0.0, 0.0, 100.0);
    }
    SECTION("out point past the end of the track")
    {
        addClip(set, 1, 0, 0.0, 0.0, 400.0);
    }
    SECTION("out point before in point")
    {
        addClip(set, 1, 0, 0.0, 50.0, 40.0);
    }
    SECTION("missing track")
    {
        addClip(set, 99, 0, 0.0, 0.0, 100.0);
    }
    SECTION("pitch beyond +/-8%")
    {
        addClip(set, 1, 0, 0.0, 0.0, 100.0, 161);
    }
    SECTION("pitch keypoint beyond +/-8%")
    {
        addClip(set, 1, 0, 0.0, 0.0, 100.0).points(Param::Pitch).push_back({frames(10.0), -200.0});
    }
    const auto issues = validate(set);
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].severity == Issue::Severity::Error);
}

TEST_CASE("Automation values slide linearly between keypoints")
{
    Clip clip;
    clip.mixer.fader = 0.8;
    CHECK(valueAt(clip, Param::Volume, 1000.0) == 0.8); // no keypoints: the starting value

    clip.points(Param::Volume) = {{1000, 1.0}, {3000, 0.0}, {3000, 0.5}};
    CHECK(valueAt(clip, Param::Volume, 0.0) == 1.0);     // held before the first
    CHECK(valueAt(clip, Param::Volume, 2000.0) == Approx(0.5));
    CHECK(valueAt(clip, Param::Volume, 3500.0) == 0.5);  // the jump at 3000 lands on the later point
    CHECK(mixerAt(clip, 2000.0).fader == Approx(0.5));
    CHECK(mixerAt(clip, 2000.0).high == 0.5);            // untouched controls keep their starting values
}

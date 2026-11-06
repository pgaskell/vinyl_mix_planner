#include "core/Document.h"
#include "core/Edits.h"
#include "core/Pitch.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>

using namespace vmp;
using Catch::Approx;

namespace
{
constexpr double kRate = 44100.0;

double frames(double seconds)
{
    return seconds * kRate;
}

// One six-minute track, played from its start at 100 s on the timeline.
Set makeSet(int pitchSteps = 0)
{
    Set set;
    set.tracks.push_back({1, "/music/a.flac", "A", static_cast<int64_t>(frames(360.0)), kRate, PressedSpeed::Unknown, {}, {}});
    Clip clip;
    clip.id = 2;
    clip.trackId = 1;
    clip.startSeconds = 100.0;
    clip.outFrame = static_cast<int64_t>(frames(300.0));
    clip.pitchSteps = pitchSteps;
    set.clips.push_back(clip);
    set.nextId = 3;
    return set;
}

ClipTiming timingOf(const Set& set)
{
    return ClipTiming(set.clips.front(), set.tracks.front());
}
} // namespace

TEST_CASE("A nudge moves the music after it and leaves the music before it alone")
{
    for (const double amount : {-0.010, 0.010})
    {
        auto set = makeSet();
        const auto before = timingOf(set);
        REQUIRE(edit::addNudge(set, 2, 130.0, amount));
        const auto after = timingOf(set);

        CHECK(after.timelineSecondsAt(frames(20.0)) == Approx(120.0));
        CHECK(after.timelineSecondsAt(frames(30.0)) == Approx(130.0));
        CHECK(after.timelineSecondsAt(frames(60.0)) == Approx(160.0 + amount));
        CHECK(after.endSeconds() == Approx(before.endSeconds() + amount));
        // At its peak it goes at the deck's slew speed; it lasts nudgeLengthSeconds on the timeline.
        CHECK(after.speedAt(130.07) == Approx(amount < 0 ? 1.0 + kNudgeSpeed : 1.0 - kNudgeSpeed));
        CHECK(after.speedAt(130.0 + nudgeLengthSeconds(amount) + 0.001) == Approx(1.0));
        CHECK(after.pitchSpeedAt(130.05) == Approx(1.0));
        for (double t = 95.0; t < 140.0; t += 0.0137)
            CHECK(after.timelineSecondsAt(after.sourceFrameAt(t)) == Approx(t).margin(1e-9));
    }
}

TEST_CASE("A nudge is the same size whatever the pitch")
{
    auto set = makeSet(300); // +6%
    const double music = timingOf(set).sourceFrameAt(160.0);
    REQUIRE(edit::addNudge(set, 2, 130.0, -0.004));
    CHECK(timingOf(set).timelineSecondsAt(music) == Approx(160.0 - 0.004));
    CHECK(timingOf(set).speedAt(130.03) == Approx(1.06 * (1.0 + kNudgeSpeed))); // at its peak
}

TEST_CASE("Nudging again while a nudge plays out adds to it")
{
    auto set = makeSet();
    REQUIRE(edit::addNudge(set, 2, 130.0, -0.010));
    REQUIRE(edit::addNudge(set, 2, 130.05, -0.010));
    REQUIRE(set.clips.front().actions.size() == 1);
    CHECK(set.clips.front().actions.front().seconds == Approx(-0.020));
    REQUIRE(edit::addNudge(set, 2, 130.1, 0.020)); // and taking it back removes it
    CHECK(set.clips.front().actions.empty());

    REQUIRE(edit::addNudge(set, 2, 130.0, -0.010));
    REQUIRE(edit::addNudge(set, 2, 140.0, 0.002)); // a separate one later
    CHECK(set.clips.front().actions.size() == 2);
    CHECK(timingOf(set).timelineSecondsAt(frames(60.0)) == Approx(160.0 - 0.008));
}

TEST_CASE("Nudges outside the clip are refused, and can be resized, deleted and cleared")
{
    auto set = makeSet();
    CHECK_FALSE(edit::addNudge(set, 2, 99.0, 0.01));
    CHECK_FALSE(edit::addNudge(set, 2, 130.0, 0.0));
    REQUIRE(edit::addNudge(set, 2, 130.0, 0.01));
    REQUIRE(edit::addNudge(set, 2, 150.0, 0.01));
    REQUIRE(edit::setActionSeconds(set, 2, 0, 5.0));
    CHECK(set.clips.front().actions[0].seconds == edit::kMaxNudgeSeconds);
    REQUIRE(edit::deleteAction(set, 2, 0));
    CHECK(set.clips.front().actions.size() == 1);
    REQUIRE(edit::clearActions(set, 2));
    CHECK(set.clips.front().actions.empty());
}

TEST_CASE("Trimming the start past a nudge keeps the music where it was")
{
    auto set = makeSet();
    set.clips.front().points(Param::Pitch) = {{static_cast<int64_t>(frames(20.0)), 100.0}};
    REQUIRE(edit::addNudge(set, 2, 125.0, -0.010));
    REQUIRE(edit::addNudge(set, 2, 150.0, 0.006));
    const auto before = timingOf(set);
    REQUIRE(edit::trimClipStart(set, 2, 140.0));
    const auto after = timingOf(set);
    CHECK(set.clips.front().pitchSteps == Approx(100.0)); // the pitch, not the nudge's bump
    for (double t = 140.0; t < 200.0; t += 0.37)
        CHECK(after.sourceFrameAt(t) == Approx(before.sourceFrameAt(t)));
}

TEST_CASE("Records mixed in later follow a nudge")
{
    auto set = makeSet();
    Clip next = set.clips.front();
    next.id = 3;
    next.deck = 1;
    next.startSeconds = 350.0; // mixes in over the end of the first
    set.clips.push_back(next);
    set.numDecks = 2;
    REQUIRE(edit::withFollowers(set, 2, [](Set& s) { return edit::addNudge(s, 2, 200.0, -0.010); }));
    CHECK(set.clips[1].startSeconds == Approx(350.0 - 0.010));
}

TEST_CASE("Moving a record or changing its pitch drops its nudges")
{
    auto set = makeSet();
    REQUIRE(edit::addNudge(set, 2, 130.0, -0.010));
    REQUIRE(edit::moveClip(set, 2, 100.0, 0)); // not moved: kept
    CHECK(set.clips.front().actions.size() == 1);
    REQUIRE(edit::moveClip(set, 2, 101.0, 0));
    CHECK(set.clips.front().actions.empty());

    // A pitch change keeps the music at the playhead where it was heard, nudge included.
    REQUIRE(edit::addNudge(set, 2, 131.0, -0.010));
    const double heard = timingOf(set).sourceFrameAt(150.0);
    REQUIRE(edit::setParamAt(set, 2, Param::Pitch, 100.0, 150.0));
    CHECK(set.clips.front().actions.empty());
    CHECK(timingOf(set).timelineSecondsAt(heard) == Approx(150.0));

    REQUIRE(edit::addNudge(set, 2, 140.0, 0.010));
    REQUIRE(edit::setParamAt(set, 2, Param::Volume, 0.5, 150.0)); // other controls leave them alone
    CHECK(set.clips.front().actions.size() == 1);
    REQUIRE(edit::setAutomationPoint(set, 2, Param::Pitch, static_cast<int64_t>(frames(80.0)), 50.0));
    CHECK(set.clips.front().actions.empty());
}

TEST_CASE("A touch held still stops the record and lets it go, the music after it that much later")
{
    auto set = makeSet();
    // Stopped by the hand over 30 ms, held to 0.5 s, then let go.
    REQUIRE(edit::addTouch(set, 2, 130.0, {{0.03, 0.0}, {0.5, 0.0}}));
    const auto& touch = set.clips.front().actions.front();
    CHECK(touch.kind == DeckAction::Kind::Touch);
    CHECK(touch.touch.back().factor == 1.0); // let go: back up to speed
    CHECK(actionOffsetSeconds(touch) == Approx(0.5));
    const auto timing = timingOf(set);
    CHECK(timing.timelineSecondsAt(frames(29.0)) == Approx(129.0));
    CHECK(timing.timelineSecondsAt(frames(60.0)) == Approx(160.5));
    const double held = timing.sourceFrameAt(130.2);
    CHECK(timing.sourceFrameAt(130.3) == Approx(held));
    CHECK(timing.speedAt(130.2) == 0.0);
    CHECK(timing.speedAt(130.01) > 0.0);
    CHECK(timing.speedAt(130.01) < 1.0);
    CHECK(timing.speedAt(130.0 + actionLengthSeconds(touch) + 0.001) == Approx(1.0));
    for (double t = 125.0; t < 135.0; t += 0.0071)
    {
        CHECK(timing.speedAt(t) <= 1.0 + 1e-9);
        CHECK(timing.sourceFrameAt(t + 0.001) >= timing.sourceFrameAt(t));
    }
}

TEST_CASE("A touch can push the record on, and is cleaned up")
{
    auto set = makeSet();
    REQUIRE(edit::addTouch(set, 2, 130.0, {{0.1, 2.0}, {0.2, 2.0}, {0.05, 9.0}, {0.3, 9.0}}));
    const auto& touch = set.clips.front().actions.front().touch;
    REQUIRE(touch.size() == 5); // the out-of-order point dropped, the end eased back to 1
    CHECK(touch[3].factor == kMaxTouchFactor);
    CHECK(touch.back().factor == 1.0);
    CHECK(timingOf(set).timelineSecondsAt(frames(60.0)) == Approx(160.0 + actionOffsetSeconds(set.clips.front().actions.front())));
    CHECK(actionOffsetSeconds(set.clips.front().actions.front()) < 0.0);
}

TEST_CASE("A nudge's speed changes how long it takes, not how far it goes")
{
    auto set = makeSet();
    REQUIRE(edit::addNudge(set, 2, 130.0, -0.010));
    REQUIRE(edit::setNudgeSpeed(set, 2, 0, 0.02));
    const auto timing = timingOf(set);
    CHECK(timing.speedAt(130.4) == Approx(1.02));
    CHECK(timing.speedAt(130.0 + nudgeLengthSeconds(-0.010, 0.02) + 0.001) == Approx(1.0));
    CHECK(timing.timelineSecondsAt(frames(60.0)) == Approx(160.0 - 0.010));
    REQUIRE(edit::setNudgeSpeed(set, 2, 0, 5.0));
    CHECK(set.clips.front().actions.front().speed == kMaxNudgeSpeed);
    REQUIRE(edit::addTouch(set, 2, 150.0, {{0.03, 0.0}, {1.0, 0.0}}));
    CHECK_FALSE(edit::setNudgeSpeed(set, 2, 1, 0.2)); // touches have no speed
}

TEST_CASE("Moving a nudge along the music keeps the music before and after it in place")
{
    auto set = makeSet();
    REQUIRE(edit::addNudge(set, 2, 140.0, -0.010));
    REQUIRE(edit::addNudge(set, 2, 150.0, 0.004));
    REQUIRE(edit::moveAction(set, 2, 0, static_cast<int64_t>(frames(32.0))));
    const auto timing = timingOf(set);
    CHECK(timing.timelineSecondsAt(frames(31.0)) == Approx(131.0));
    CHECK(timing.timelineSecondsAt(frames(45.0)) == Approx(145.0 - 0.010));
    CHECK(timing.timelineSecondsAt(frames(60.0)) == Approx(160.0 - 0.006));
    // Not past its neighbour, nor out of the clip.
    REQUIRE(edit::moveAction(set, 2, 0, static_cast<int64_t>(frames(70.0))));
    CHECK(set.clips.front().actions[0].sourceFrame == set.clips.front().actions[1].sourceFrame);
    REQUIRE(edit::moveAction(set, 2, 0, -1000));
    CHECK(set.clips.front().actions[0].sourceFrame == 0);
}

TEST_CASE("Overlapping actions add up")
{
    auto set = makeSet();
    REQUIRE(edit::addTouch(set, 2, 130.0, {{0.03, 0.0}, {1.0, 0.0}}));
    // A nudge whose music comes during the touch's let-go, so the two overlap.
    set.clips.front().actions.push_back({static_cast<int64_t>(frames(30.0)) + 10, DeckAction::Kind::Nudge, -0.010, 0.1, {}});
    const auto timing = timingOf(set);
    const double total = actionOffsetSeconds(set.clips.front().actions[0]) - 0.010;
    CHECK(timing.timelineSecondsAt(frames(60.0)) == Approx(160.0 + total));
    for (double t = 129.0; t < 133.0; t += 0.0013)
        CHECK(timing.sourceFrameAt(t + 0.001) >= timing.sourceFrameAt(t));
}

TEST_CASE("A record with many touches still times quickly")
{
    // Built on every edit and, before the cache, on every frame: it has to stay linear.
    auto set = makeSet();
    auto& clip = set.clips.front();
    for (int i = 0; i < 200; ++i)
    {
        DeckAction touch;
        touch.kind = DeckAction::Kind::Touch;
        touch.sourceFrame = static_cast<int64_t>(frames(1.0 + i * 1.2));
        touch.touch = edit::cleanTouch({{0.03, 0.0}, {0.5, 0.0}, {1.0, 0.8}});
        clip.actions.push_back(touch);
    }
    const auto start = std::chrono::steady_clock::now();
    const auto timing = timingOf(set);
    const auto took = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(took < 0.25); // ~40,000 breakpoints; quadratic took minutes
    CHECK(timing.endSeconds() > 0.0);
}

TEST_CASE("A hold inserted by hand moves the music after it by its length")
{
    auto set = makeSet();
    REQUIRE(edit::addHold(set, 2, 130.0, 0.4));
    CHECK(actionOffsetSeconds(set.clips.front().actions.front()) == Approx(0.4));
    CHECK(timingOf(set).timelineSecondsAt(frames(60.0)) == Approx(160.4));
}

TEST_CASE("Cleaning up nudges makes them one move at the start of the stretch")
{
    auto set = makeSet();
    // Late and the wrong way first: pushed, then held back more, then a touch.
    REQUIRE(edit::addNudge(set, 2, 140.0, -0.010));
    REQUIRE(edit::addNudge(set, 2, 142.0, 0.020));
    REQUIRE(edit::addNudge(set, 2, 143.0, 0.010));
    REQUIRE(edit::addHold(set, 2, 150.0, 0.03));
    REQUIRE(edit::addNudge(set, 2, 170.0, -0.005)); // outside the stretch: kept
    const double after = timingOf(set).sourceFrameAt(200.0);

    const auto net = edit::tidyActions(set, 2, 135.0, 160.0);
    REQUIRE(net);
    CHECK(*net == Approx(0.05));
    const auto& actions = set.clips.front().actions;
    REQUIRE(actions.size() == 2);
    CHECK(actionOffsetSeconds(actions[0]) == Approx(0.05));
    CHECK(timingOf(set).timelineSecondsAt(static_cast<double>(actions[0].sourceFrame)) == Approx(135.0));
    CHECK(timingOf(set).sourceFrameAt(200.0) == Approx(after).margin(1.0)); // the music after: unchanged
    CHECK(timingOf(set).sourceFrameAt(134.0) == Approx(frames(34.0)));     // and before

    // Ones that cancel out just go; a big one becomes a hold.
    REQUIRE(edit::addNudge(set, 2, 180.0, -0.010));
    REQUIRE(edit::addNudge(set, 2, 181.0, 0.010));
    REQUIRE(edit::tidyActions(set, 2, 175.0, 185.0));
    CHECK(set.clips.front().actions.size() == 2);
    REQUIRE(edit::addHold(set, 2, 200.0, 0.4));
    REQUIRE(edit::addHold(set, 2, 210.0, 0.4));
    REQUIRE(edit::tidyActions(set, 2, 195.0, 215.0));
    CHECK(set.clips.front().actions.back().kind == DeckAction::Kind::Touch);
    CHECK(actionOffsetSeconds(set.clips.front().actions.back()) == Approx(0.8));
    CHECK_FALSE(edit::tidyActions(set, 2, 250.0, 260.0)); // nothing there
}

TEST_CASE("Setting how far an action moves the music")
{
    auto set = makeSet();
    REQUIRE(edit::addNudge(set, 2, 130.0, -0.010));
    REQUIRE(edit::setActionOffset(set, 2, 0, -0.0042));
    CHECK(timingOf(set).timelineSecondsAt(frames(60.0)) == Approx(160.0 - 0.0042));
    REQUIRE(edit::setActionOffset(set, 2, 0, 0.0)); // through zero: kept, doing nothing
    CHECK(set.clips.front().actions.size() == 1);
    CHECK(timingOf(set).timelineSecondsAt(frames(60.0)) == Approx(160.0));
    REQUIRE(edit::setActionOffset(set, 2, 0, 0.003));
    CHECK(timingOf(set).timelineSecondsAt(frames(60.0)) == Approx(160.003));

    REQUIRE(edit::addHold(set, 2, 150.0, 0.4));
    REQUIRE(edit::setActionOffset(set, 2, 1, 0.25));
    CHECK(actionOffsetSeconds(set.clips.front().actions[1]) == Approx(0.25));
    CHECK_FALSE(edit::setActionOffset(set, 2, 1, -0.1)); // a hold can't become a push
}

namespace
{
// makeSet's A from 100 s on deck 1, and B from 200 s on deck 2.
Set makeMix()
{
    auto set = makeSet();
    set.tracks.push_back({3, "/music/b.flac", "B", static_cast<int64_t>(frames(360.0)), kRate, PressedSpeed::Unknown, {}, {}});
    Clip b;
    b.id = 4;
    b.trackId = 3;
    b.deck = 1;
    b.startSeconds = 200.0;
    b.outFrame = static_cast<int64_t>(frames(300.0));
    set.clips.push_back(b);
    set.nextId = 5;
    return set;
}

int64_t at(double seconds)
{
    return static_cast<int64_t>(frames(seconds));
}

double linkErrorOf(const Set& set, size_t index)
{
    return *edit::linkError(set, set.links[index]);
}
} // namespace

TEST_CASE("Lining up beats links a record's next beat to the other's")
{
    // Both 120 BPM, B's beats 13 ms late (or a little slower too).
    for (const double bpm : {120.0, 120.05})
    {
        auto mix = makeMix();
        const BeatGrid gridA{120.0, 0.0};
        const BeatGrid gridB{bpm, 0.013};
        CHECK(std::abs(*edit::beatOffset(mix, 4, gridB, 2, gridA, 250.0)) > 0.005);
        REQUIRE(edit::lineUpBeats(mix, 4, gridB, 2, gridA, 250.0));
        REQUIRE(mix.links.size() == 1);
        CHECK(mix.links[0].leader == 2); // A was playing first
        edit::solveLinks(mix);
        CHECK(std::abs(linkErrorOf(mix, 0)) < 1e-6);
        CHECK(std::abs(*edit::beatOffset(mix, 4, gridB, 2, gridA, 250.3)) < 0.0001); // the linked beat, at 250.5 s
    }
    // Only where both play.
    auto mix = makeMix();
    CHECK_FALSE(edit::lineUpBeats(mix, 4, BeatGrid{120.0, 0.0}, 2, BeatGrid{120.0, 0.0}, 150.0));
}

TEST_CASE("A beat link places the record mixed in later")
{
    auto mix = makeMix();
    // A's 150 s with B's 10 s: B drops at 240 s.
    REQUIRE(edit::addLink(mix, 4, at(10.0), 2, at(150.0)));
    CHECK(mix.links[0].leader == 2);
    CHECK(mix.links[0].follower == 4);
    edit::solveLinks(mix);
    CHECK(mix.clips[1].startSeconds == Approx(240.0));
    CHECK(mix.clips[0].startSeconds == 100.0);

    // Solved already: nothing changes.
    const auto solved = mix;
    edit::solveLinks(mix);
    CHECK(mix == solved);

    // Moving A takes B along.
    REQUIRE(edit::moveClip(mix, 2, 105.0, 0));
    edit::solveLinks(mix);
    CHECK(mix.clips[1].startSeconds == Approx(245.0));

    // Refused: the same deck, or outside the music either plays.
    CHECK_FALSE(edit::addLink(mix, 2, at(10.0), 2, at(150.0)));
    CHECK_FALSE(edit::addLink(mix, 4, at(310.0), 2, at(150.0)));
}

TEST_CASE("Two beat links set the pitch and nudge out what the steps can't")
{
    auto mix = makeMix();
    // B has to play 30.31 s of its music in A's 30 s: about +1.03%, between two steps.
    REQUIRE(edit::addLink(mix, 2, at(150.0), 4, at(10.0)));
    REQUIRE(edit::addLink(mix, 2, at(180.0), 4, at(40.31)));
    edit::solveLinks(mix);
    const Clip& b = mix.clips[1];
    CHECK(b.pitchSteps == 52.0);
    REQUIRE(b.actions.size() == 1);
    CHECK(b.actions[0].linked);
    CHECK(std::abs(linkErrorOf(mix, 0)) < 1e-6);
    CHECK(std::abs(linkErrorOf(mix, 1)) < 1e-6);
    CHECK(validate(mix).empty());

    // A nudge by hand between them stays; the link's nudge makes up for it.
    REQUIRE(edit::addNudge(mix, 4, 260.0, 0.010));
    edit::solveLinks(mix);
    CHECK(mix.clips[1].actions.size() == 2);
    CHECK(std::abs(linkErrorOf(mix, 1)) < 1e-6);

    // Crossing (later in B, earlier in A) is refused.
    CHECK_FALSE(edit::addLink(mix, 2, at(170.0), 4, at(50.0)));

    // Too far for the pitch fader: kept as far as it goes, and reported.
    auto far = mix;
    REQUIRE(edit::moveLink(far, far.links[1].id, 4, at(50.0)));
    edit::solveLinks(far);
    CHECK(far.clips[1].pitchSteps == kMaxPitchSteps);
    CHECK_FALSE(validate(far).empty());

    // Deleting A drops its links and the nudges they wrote.
    REQUIRE(edit::deleteClip(mix, 2));
    edit::solveLinks(mix);
    CHECK(mix.links.empty());
    REQUIRE(mix.clips.size() == 1);
    CHECK(mix.clips[0].actions.size() == 1);
    CHECK_FALSE(mix.clips[0].actions[0].linked);
}

TEST_CASE("Beat links ride the pitch where the music changes tempo")
{
    auto mix = makeMix();
    // B plays 30.31 s of its music in A's first 30 s (+1.03%), then 30.6 s in the next 30 (+2%).
    REQUIRE(edit::addLink(mix, 2, at(150.0), 4, at(10.0)));
    REQUIRE(edit::addLink(mix, 2, at(180.0), 4, at(40.31)));
    REQUIRE(edit::addLink(mix, 2, at(210.0), 4, at(70.91)));
    edit::solveLinks(mix);
    const Clip& b = mix.clips[1];
    CHECK(b.pitchSteps == 52.0);
    const auto& points = b.points(Param::Pitch);
    REQUIRE(points.size() == 2);
    CHECK(points[0].sourceFrame == at(40.31));
    CHECK(points[0].value == 52.0);
    CHECK(points[1].value == 100.0);
    for (size_t i = 0; i < mix.links.size(); ++i)
        CHECK(std::abs(linkErrorOf(mix, i)) < 1e-6);
    CHECK(validate(mix).empty());

    // A link that only disagrees by a few ms is nudged, not ridden.
    REQUIRE(edit::moveLink(mix, mix.links[2].id, 4, at(70.62 + 0.02)));
    edit::solveLinks(mix);
    CHECK(mix.clips[1].points(Param::Pitch).empty());
    CHECK(mix.clips[1].pitchSteps == 53.0);
    CHECK(std::abs(linkErrorOf(mix, 2)) < 1e-6);
}

TEST_CASE("Every edit to the set keeps its beat links")
{
    SetDocument document;
    document.reset(makeMix());
    REQUIRE(document.edit([](Set& s) { return edit::addLink(s, 2, at(150.0), 4, at(10.0)).has_value(); }));
    CHECK(document.set().clips[1].startSeconds == Approx(240.0));
    CHECK_FALSE(document.edit([](Set& s) { return edit::moveClip(s, 4, 230.0, 1); })); // the link wins
    CHECK(document.set().clips[1].startSeconds == Approx(240.0));
    REQUIRE(document.edit([](Set& s) { return edit::deleteLinks(s, 4); }));
    REQUIRE(document.edit([](Set& s) { return edit::moveClip(s, 4, 230.0, 1); }));
    CHECK(document.set().clips[1].startSeconds == 230.0);
}

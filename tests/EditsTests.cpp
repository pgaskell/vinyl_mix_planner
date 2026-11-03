#include "core/Document.h"
#include "core/Edits.h"
#include "core/Pitch.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace vmp;
using Catch::Approx;

namespace
{
constexpr double kRate = 44100.0;

int64_t frames(double seconds)
{
    return static_cast<int64_t>(seconds * kRate);
}

Set makeSet()
{
    Set set;
    edit::addTrack(set, {0, "/music/a.flac", "A", frames(300.0), kRate, PressedSpeed::Unknown, {}, {}});
    return set;
}

const Clip& onlyClip(const Set& set)
{
    REQUIRE(set.clips.size() == 1);
    return set.clips.front();
}
} // namespace

TEST_CASE("Adding the same file twice reuses the track")
{
    auto set = makeSet();
    const Id again = edit::addTrack(set, {0, "/music/a.flac", "A", frames(300.0), kRate, PressedSpeed::Unknown, {}, {}});
    CHECK(again == set.tracks.front().id);
    CHECK(set.tracks.size() == 1);
}

TEST_CASE("A new clip covers the whole track")
{
    auto set = makeSet();
    const Id id = edit::addClip(set, set.tracks.front().id, 1, 30.0);
    REQUIRE(id != 0);
    const auto& clip = onlyClip(set);
    CHECK(clip.inFrame == 0);
    CHECK(clip.outFrame == frames(300.0));
    CHECK(clip.deck == 1);
    CHECK(edit::addClip(set, set.tracks.front().id, 2, 0.0) == 0); // only 2 decks
}

TEST_CASE("Deleting the last clip of a track removes the track")
{
    auto set = makeSet();
    const Id track = set.tracks.front().id;
    const Id a = edit::addClip(set, track, 0, 0.0);
    const Id b = edit::addClip(set, track, 1, 0.0);
    CHECK(edit::deleteClip(set, a));
    CHECK(set.tracks.size() == 1);
    CHECK(edit::deleteClip(set, b));
    CHECK(set.tracks.empty());
}

TEST_CASE("Trimming the start keeps the music in place")
{
    auto set = makeSet();
    const Id id = edit::addClip(set, set.tracks.front().id, 0, 100.0);
    REQUIRE(edit::setClipPitch(set, id, 200)); // +4%

    REQUIRE(edit::trimClipStart(set, id, 110.0));
    const auto& clip = onlyClip(set);
    // 10 s of timeline at +4% is 10.4 s of music.
    CHECK(static_cast<double>(clip.inFrame) == Approx(10.4 * kRate).margin(1.0));
    CHECK(clip.startSeconds == Approx(110.0).margin(1e-4));

    // Trimming back out restores the earlier music, but not before the start of the record.
    REQUIRE(edit::trimClipStart(set, id, 0.0));
    CHECK(onlyClip(set).inFrame == 0);
    CHECK(onlyClip(set).startSeconds == Approx(100.0).margin(1e-6));
}

TEST_CASE("Trimming the start past pitch keypoints keeps the music in place")
{
    auto set = makeSet();
    const Id id = edit::addClip(set, set.tracks.front().id, 0, 0.0);
    set.clips.front().points(Param::Pitch) = {{frames(60.0), 0.0}, {frames(70.0), 250.0}};
    const ClipTiming before(onlyClip(set), set.tracks.front());
    const double later = before.timelineSecondsAt(static_cast<double>(frames(100.0)));

    REQUIRE(edit::trimClipStart(set, id, before.timelineSecondsAt(static_cast<double>(frames(65.0)))));
    const ClipTiming after(onlyClip(set), set.tracks.front());
    CHECK(after.timelineSecondsAt(static_cast<double>(frames(100.0))) == Approx(later).margin(1e-4));
}

TEST_CASE("Keypoints are added, replaced, moved between neighbours and deleted")
{
    auto set = makeSet();
    const Id id = edit::addClip(set, set.tracks.front().id, 0, 0.0);
    auto points = [&] { return onlyClip(set).points(Param::Volume); };

    REQUIRE(edit::setAutomationPoint(set, id, Param::Volume, 2000, 0.4));
    REQUIRE(edit::setAutomationPoint(set, id, Param::Volume, 1000, 1.7)); // clamped, sorted in
    REQUIRE(edit::setAutomationPoint(set, id, Param::Volume, 2000, 0.2)); // replaces
    REQUIRE(points().size() == 2);
    CHECK(points()[0] == AutomationPoint{1000, 1.0});
    CHECK(points()[1] == AutomationPoint{2000, 0.2});

    REQUIRE(edit::moveAutomationPoint(set, id, Param::Volume, 0, 5000, 0.5)); // can't pass its neighbour
    CHECK(points()[0] == AutomationPoint{2000, 0.5});

    REQUIRE(edit::deleteAutomationPoint(set, id, Param::Volume, 1));
    CHECK(points().size() == 1);
    CHECK_FALSE(edit::deleteAutomationPoint(set, id, Param::Volume, 5));
}

TEST_CASE("Pitch keypoints are whole steps within +/-8%")
{
    auto set = makeSet();
    const Id id = edit::addClip(set, set.tracks.front().id, 0, 0.0);
    REQUIRE(edit::setAutomationPoint(set, id, Param::Pitch, 1000, 12.4));
    REQUIRE(edit::setAutomationPoint(set, id, Param::Pitch, 2000, 2000.0));
    CHECK(onlyClip(set).points(Param::Pitch)[0].value == 12.0);
    CHECK(onlyClip(set).points(Param::Pitch)[1].value == kMaxPitchSteps);
}

TEST_CASE("Clearing automation keeps the value the clip started with")
{
    auto set = makeSet();
    const Id id = edit::addClip(set, set.tracks.front().id, 0, 0.0);
    edit::setAutomationPoint(set, id, Param::Low, 0, 0.1);
    edit::setAutomationPoint(set, id, Param::Low, 50000, 0.9);
    REQUIRE(edit::clearAutomation(set, id, Param::Low));
    CHECK(onlyClip(set).points(Param::Low).empty());
    CHECK(onlyClip(set).mixer.low == Approx(0.1));
}

TEST_CASE("A control without keypoints sets the starting value; with keypoints it writes at the playhead")
{
    auto set = makeSet();
    const Id id = edit::addClip(set, set.tracks.front().id, 0, 10.0);

    REQUIRE(edit::setParamAt(set, id, Param::Mid, 0.3, 20.0));
    CHECK(onlyClip(set).mixer.mid == 0.3);
    CHECK(onlyClip(set).points(Param::Mid).empty());

    edit::setAutomationPoint(set, id, Param::Mid, 0, 0.5);
    REQUIRE(edit::setParamAt(set, id, Param::Mid, 0.9, 20.0)); // 10 s into the record
    REQUIRE(onlyClip(set).points(Param::Mid).size() == 2);
    CHECK(onlyClip(set).points(Param::Mid)[1] == AutomationPoint{frames(10.0), 0.9});

    // Close to an existing keypoint, it updates that one rather than adding another.
    REQUIRE(edit::setParamAt(set, id, Param::Mid, 0.7, 20.005));
    REQUIRE(onlyClip(set).points(Param::Mid).size() == 2);
    CHECK(onlyClip(set).points(Param::Mid)[1].value == 0.7);

    CHECK_FALSE(edit::setParamAt(set, id, Param::Mid, 0.2, 5.0)); // playhead outside the clip
}

TEST_CASE("Writing pitch automation keeps the music under the playhead in place")
{
    auto set = makeSet();
    const Id id = edit::addClip(set, set.tracks.front().id, 0, 10.0);
    edit::setAutomationPoint(set, id, Param::Pitch, 0, 0.0);
    const double frameAtPlayhead = ClipTiming(onlyClip(set), set.tracks.front()).sourceFrameAt(70.0);

    REQUIRE(edit::setParamAt(set, id, Param::Pitch, 200.0, 70.0));
    CHECK(ClipTiming(onlyClip(set), set.tracks.front()).sourceFrameAt(70.0) == Approx(frameAtPlayhead).margin(0.5));
}

TEST_CASE("Trimming the end is limited by the record")
{
    auto set = makeSet();
    const Id id = edit::addClip(set, set.tracks.front().id, 0, 10.0);
    REQUIRE(edit::trimClipEnd(set, id, 70.0));
    CHECK(onlyClip(set).outFrame == frames(60.0));
    REQUIRE(edit::trimClipEnd(set, id, 1000.0));
    CHECK(onlyClip(set).outFrame == frames(300.0));
    REQUIRE(edit::trimClipEnd(set, id, 0.0));
    CHECK(static_cast<double>(onlyClip(set).outFrame) == Approx(edit::kMinClipSeconds * kRate).margin(1.0));
}

TEST_CASE("Pitch and mixer values are clamped to the hardware's range")
{
    auto set = makeSet();
    const Id id = edit::addClip(set, set.tracks.front().id, 0, 0.0);
    REQUIRE(edit::setClipPitch(set, id, 999));
    CHECK(onlyClip(set).pitchSteps == kMaxPitchSteps);
    REQUIRE(edit::setClipMixer(set, id, {1.5, -0.2, 0.5, 0.25}));
    CHECK(onlyClip(set).mixer == ChannelSettings{1.0, 0.0, 0.5, 0.25});
    REQUIRE(edit::setClipMixer(set, id, {1.0, 0.5, 0.5, 0.5, 0.5, -1.0}));
    CHECK(onlyClip(set).mixer.color == 0.0);
    REQUIRE(edit::setClipTrim(set, id, 1.2));
    CHECK(onlyClip(set).mixer.trim == 1.0);
    CHECK_FALSE(edit::setClipTrim(set, id + 99, 0.5));
}

TEST_CASE("Decks can't be removed while clips are on them")
{
    auto set = makeSet();
    edit::addClip(set, set.tracks.front().id, 1, 0.0);
    CHECK(edit::setNumDecks(set, 4));
    CHECK_FALSE(edit::setNumDecks(set, 1));
    CHECK(set.numDecks == 4);
}

TEST_CASE("Undo and redo restore whole states")
{
    SetDocument doc;
    doc.reset(makeSet());
    CHECK_FALSE(doc.isDirty());

    Id id = 0;
    REQUIRE(doc.edit([&](Set& s) { return (id = edit::addClip(s, s.tracks.front().id, 0, 0.0)) != 0; }));
    REQUIRE(doc.edit([&](Set& s) { return edit::moveClip(s, id, 50.0, 1); }));
    CHECK(doc.isDirty());

    doc.undo();
    CHECK(doc.set().clips.front().startSeconds == 0.0);
    doc.undo();
    CHECK(doc.set().clips.empty());
    CHECK_FALSE(doc.isDirty()); // back where we started
    doc.redo();
    doc.redo();
    CHECK(doc.set().clips.front().startSeconds == 50.0);
}

TEST_CASE("A gesture undoes as one step")
{
    SetDocument doc;
    auto set = makeSet();
    const Id id = edit::addClip(set, set.tracks.front().id, 0, 0.0);
    doc.reset(set);

    doc.beginGesture();
    for (double t = 1.0; t <= 10.0; t += 1.0)
        doc.edit([&](Set& s) { return edit::moveClip(s, id, t, 0); });
    doc.endGesture();

    CHECK(doc.set().clips.front().startSeconds == 10.0);
    doc.undo();
    CHECK(doc.set().clips.front().startSeconds == 0.0);
    CHECK_FALSE(doc.canUndo());
}

TEST_CASE("Edits that change nothing are not recorded")
{
    SetDocument doc;
    auto set = makeSet();
    const Id id = edit::addClip(set, set.tracks.front().id, 0, 0.0);
    doc.reset(set);
    const auto revision = doc.revision();
    CHECK_FALSE(doc.edit([&](Set& s) { return edit::moveClip(s, id, 0.0, 0); }));
    CHECK_FALSE(doc.edit([&](Set& s) { return edit::moveClip(s, 999, 5.0, 0); }));
    CHECK(doc.revision() == revision);
    CHECK_FALSE(doc.canUndo());
}

TEST_CASE("Saving marks the current state clean, and undoing past it is dirty again")
{
    SetDocument doc;
    doc.reset(makeSet());
    doc.edit([&](Set& s) { return edit::addClip(s, s.tracks.front().id, 0, 0.0) != 0; });
    doc.markSaved();
    CHECK_FALSE(doc.isDirty());
    doc.undo();
    CHECK(doc.isDirty());
    doc.redo();
    CHECK_FALSE(doc.isDirty());
}

TEST_CASE("Changing pitch with a pivot keeps the music under the playhead in place")
{
    auto set = makeSet();
    const Id id = edit::addClip(set, set.tracks.front().id, 0, 100.0);

    // 60 s into the clip the record is at 60 s. At +4% it must still be there at 160 s.
    REQUIRE(edit::setClipPitch(set, id, 200, 160.0)); // +4%
    const auto& clip = onlyClip(set);
    CHECK(clip.startSeconds == Approx(160.0 - 60.0 / 1.04));
    const ClipTiming timing(clip, *set.findTrack(clip.trackId));
    CHECK(timing.sourceFrameAt(160.0) == Approx(60.0 * kRate));

    // Outside the clip the needle drop stays put.
    REQUIRE(edit::setClipPitch(set, id, 0, 5.0));
    CHECK(onlyClip(set).startSeconds == Approx(160.0 - 60.0 / 1.04));
}

TEST_CASE("Grid and key corrections are stored on the track and can be cleared")
{
    auto set = makeSet();
    const Id track = set.tracks.front().id;
    REQUIRE(edit::setTrackGrid(set, track, BeatGrid{124.0, 10.0}));
    // Stored as the equivalent phrase start within the first 16 bars.
    const double phrase = 64.0 * 60.0 / 124.0;
    CHECK(set.tracks.front().grid->firstBeatSeconds == Approx(10.0 - std::floor(10.0 / phrase) * phrase));
    CHECK_FALSE(edit::setTrackGrid(set, track, BeatGrid{0.0, 0.0}));
    REQUIRE(edit::setTrackGrid(set, track, std::nullopt));
    CHECK_FALSE(set.tracks.front().grid);

    REQUIRE(edit::setTrackKey(set, track, MusicalKey{9, true}));
    CHECK(set.tracks.front().key == MusicalKey{9, true});
}

TEST_CASE("Trimming the start into a slide keeps the automation sounding the same")
{
    auto set = makeSet();
    const Id id = edit::addClip(set, set.tracks.front().id, 0, 0.0);
    edit::setAutomationPoint(set, id, Param::Volume, frames(10.0), 0.0);
    edit::setAutomationPoint(set, id, Param::Volume, frames(30.0), 1.0);
    const Clip before = onlyClip(set);

    REQUIRE(edit::trimClipStart(set, id, 20.0)); // half way up the fade
    CHECK(onlyClip(set).mixer.fader == Approx(0.5));
    for (double seconds : {20.0, 25.0, 35.0})
        CHECK(valueAt(onlyClip(set), Param::Volume, seconds * kRate) == Approx(valueAt(before, Param::Volume, seconds * kRate)));
}

TEST_CASE("The starting point can be moved, and a control at the needle drop moves it")
{
    auto set = makeSet();
    const Id id = edit::addClip(set, set.tracks.front().id, 0, 10.0);
    REQUIRE(edit::setStartValue(set, id, Param::High, 0.25));
    CHECK(onlyClip(set).mixer.high == 0.25);

    edit::setAutomationPoint(set, id, Param::High, frames(5.0), 0.75);
    REQUIRE(edit::setParamAt(set, id, Param::High, 0.1, 10.0)); // playhead at the needle drop
    CHECK(onlyClip(set).mixer.high == 0.1);
    CHECK(onlyClip(set).points(Param::High).size() == 1);
}

namespace
{
// A chain of 5-minute records mixed with 30 s overlaps: before (deck 1) at 0 s, then the edited
// clip (deck 0) at 270 s, its incoming (deck 1) at 540 s, the next on deck 0 at 810 s, and one
// after a 20 s gap at 1130 s.
struct Chain
{
    Set set;
    Id before, edited, incoming, next, afterGap;

    Chain()
    {
        const Id track = edit::addTrack(set, {0, "/a.flac", "A", frames(300.0), kRate, PressedSpeed::Unknown, {}, {}});
        before = edit::addClip(set, track, 1, 0.0);
        edited = edit::addClip(set, track, 0, 270.0);
        incoming = edit::addClip(set, track, 1, 540.0);
        next = edit::addClip(set, track, 0, 810.0);
        afterGap = edit::addClip(set, track, 1, 1130.0);
        edit::setClipPitch(set, incoming, 50); // +1%
    }

    const Clip& clip(Id id) const
    {
        return *std::find_if(set.clips.begin(), set.clips.end(), [id](const Clip& c) { return c.id == id; });
    }
    ClipTiming timing(Id id) const { return ClipTiming(clip(id), set.tracks.front()); }
};
} // namespace

TEST_CASE("Nudging a clip carries the clips after it, and not the ones before")
{
    Chain c;
    const Set original = c.set;
    REQUIRE(edit::withFollowers(c.set, c.edited, [&](Set& s) {
        return edit::moveClip(s, c.edited, 270.05, 0);
    }));
    CHECK(c.clip(c.before) == Chain{}.clip(c.before));
    for (Id id : {c.incoming, c.next, c.afterGap})
    {
        const auto& was = *std::find_if(original.clips.begin(), original.clips.end(), [id](const Clip& x) { return x.id == id; });
        CHECK(c.clip(id).startSeconds == Approx(was.startSeconds + 0.05));
        CHECK(c.clip(id).pitchSteps == was.pitchSteps);
    }
}

TEST_CASE("Changing a clip's tempo changes the incoming record's tempo and keeps their music together")
{
    Chain c;
    const auto editedBefore = c.timing(c.edited), incomingBefore = c.timing(c.incoming);
    const double anchor = 555.0; // middle of the overlap
    const double editedFrame = editedBefore.sourceFrameAt(anchor), incomingFrame = incomingBefore.sourceFrameAt(anchor);

    REQUIRE(edit::withFollowers(c.set, c.edited, [&](Set& s) { return edit::setClipPitch(s, c.edited, 200); }));

    // +4% on the edited record: the incoming one goes from +1% to (1.01 * 1.04) = +5.04%.
    CHECK(c.clip(c.incoming).pitchSteps == 252);
    const auto editedNow = c.timing(c.edited), incomingNow = c.timing(c.incoming);
    const double anchorNow = editedNow.timelineSecondsAt(editedFrame);
    CHECK(incomingNow.timelineSecondsAt(incomingFrame) == Approx(anchorNow).margin(1e-6));
    // Still together at the ends of the overlap: within a millisecond after rounding to a step.
    for (double t : {incomingNow.startSeconds() + 1.0, editedNow.endSeconds() - 1.0})
    {
        const double drift = (editedNow.sourceFrameAt(t) - editedFrame) / editedNow.speedAt(t) -
                             (incomingNow.sourceFrameAt(t) - incomingFrame) / incomingNow.speedAt(t);
        CHECK(std::abs(drift / kRate) < 0.002);
    }

    // The one after the gap keeps its 20 s gap, and its tempo.
    const auto nextNow = c.timing(c.next);
    CHECK(c.timing(c.afterGap).startSeconds() == Approx(nextNow.endSeconds() + 20.0));
    CHECK(c.clip(c.afterGap).pitchSteps == 0);
}

TEST_CASE("Pitch automation on a follower shifts with it")
{
    Chain c;
    edit::setAutomationPoint(c.set, c.incoming, Param::Pitch, frames(100.0), 100); // +2%
    REQUIRE(edit::withFollowers(c.set, c.edited, [&](Set& s) { return edit::setClipPitch(s, c.edited, -100); }));
    // -2%: +1% becomes 1.01 * 0.98 = -1.02% -> -51 steps, a shift of -101.
    CHECK(c.clip(c.incoming).pitchSteps == -51);
    CHECK(c.clip(c.incoming).points(Param::Pitch).front().value == -1);
}

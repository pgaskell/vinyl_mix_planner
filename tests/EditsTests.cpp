#include "core/Document.h"
#include "core/Edits.h"

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
    REQUIRE(edit::setClipPitch(set, id, 80)); // +4%

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

TEST_CASE("Start trim stops at the first pitch move")
{
    auto set = makeSet();
    const Id id = edit::addClip(set, set.tracks.front().id, 0, 0.0);
    set.clips.front().pitchMoves.push_back({frames(60.0), 4.0, 100});
    REQUIRE(edit::trimClipStart(set, id, 90.0));
    CHECK(onlyClip(set).inFrame == frames(60.0));
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
    CHECK(onlyClip(set).pitchSteps == 160);
    REQUIRE(edit::setClipMixer(set, id, {1.5, -0.2, 0.5, 0.25}));
    CHECK(onlyClip(set).mixer == ChannelSettings{1.0, 0.0, 0.5, 0.25});
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
    REQUIRE(edit::setClipPitch(set, id, 80, 160.0));
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
    // Stored as the equivalent downbeat within the first bar.
    const double bar = 4.0 * 60.0 / 124.0;
    CHECK(set.tracks.front().grid->firstBeatSeconds == Approx(10.0 - std::floor(10.0 / bar) * bar));
    CHECK_FALSE(edit::setTrackGrid(set, track, BeatGrid{0.0, 0.0}));
    REQUIRE(edit::setTrackGrid(set, track, std::nullopt));
    CHECK_FALSE(set.tracks.front().grid);

    REQUIRE(edit::setTrackKey(set, track, MusicalKey{9, true}));
    CHECK(set.tracks.front().key == MusicalKey{9, true});
}

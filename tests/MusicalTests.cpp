#include "core/Musical.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace vmp;
using Catch::Approx;

TEST_CASE("Key names and Camelot codes")
{
    CHECK(MusicalKey{0, false}.name() == "C");
    CHECK(MusicalKey{0, false}.camelot() == "8B");
    CHECK(MusicalKey{9, true}.name() == "Am");
    CHECK(MusicalKey{9, true}.camelot() == "8A");
    CHECK(MusicalKey{7, false}.camelot() == "9B");  // G
    CHECK(MusicalKey{11, false}.camelot() == "1B"); // B
    CHECK(MusicalKey{5, false}.camelot() == "7B");  // F
    CHECK(MusicalKey{4, true}.camelot() == "9A");   // Em
    CHECK(MusicalKey{8, true}.camelot() == "1A");   // G#m
}

TEST_CASE("Every key round-trips through its name and its Camelot code")
{
    for (int tonic = 0; tonic < 12; ++tonic)
        for (bool minor : {false, true})
        {
            const MusicalKey key{tonic, minor};
            INFO(key.name());
            CHECK(MusicalKey::parse(key.name()) == key);
            CHECK(MusicalKey::parse(key.camelot()) == key);
        }
}

TEST_CASE("Parsing accepts alternative spellings and rejects junk")
{
    CHECK(MusicalKey::parse("C#") == MusicalKey{1, false});
    CHECK(MusicalKey::parse("Gb") == MusicalKey{6, false});
    CHECK(MusicalKey::parse("a#m") == MusicalKey{10, true});
    CHECK(MusicalKey::parse("12b") == MusicalKey{4, false}); // E
    CHECK_FALSE(MusicalKey::parse(""));
    CHECK_FALSE(MusicalKey::parse("H"));
    CHECK_FALSE(MusicalKey::parse("13A"));
    CHECK_FALSE(MusicalKey::parse("Cmaj"));
}

TEST_CASE("Pitching a record moves its key")
{
    const MusicalKey am{9, true};
    CHECK(pitchedKey(am, 1.0).key == am);
    CHECK(pitchedKey(am, 1.0).cents == Approx(0.0));

    // +6% is 1.009 semitones: A#m, a cent sharp.
    const auto up = pitchedKey(am, 1.06);
    CHECK(up.key == MusicalKey{10, true});
    CHECK(up.cents == Approx(0.88).margin(0.1));

    // Around half a semitone the nearer key is reported, never more than 50 cents off.
    CHECK(std::abs(pitchedKey(am, 1.03).cents) <= 50.0);

    const auto down = pitchedKey(am, 0.92); // -1.44 semitones: G#m, 44 cents flat
    CHECK(down.key == MusicalKey{8, true});
    CHECK(down.cents == Approx(-44.4).margin(0.5));
}

TEST_CASE("Harmonic compatibility follows the Camelot wheel")
{
    const MusicalKey am{9, true}, c{0, false}, em{4, true}, dm{2, true}, fsharp{6, false}, gm{7, true};
    CHECK(harmony(am, am) == Harmony::Same);
    CHECK(harmony(am, c) == Harmony::Compatible);  // 8A / 8B
    CHECK(harmony(am, em) == Harmony::Compatible); // 8A / 9A
    CHECK(harmony(am, dm) == Harmony::Compatible); // 8A / 7A
    CHECK(harmony(am, fsharp) == Harmony::Clash);
    CHECK(harmony(am, gm) == Harmony::Clash);      // 8A / 6A
    CHECK(harmony(MusicalKey{11, false}, MusicalKey{4, false}) == Harmony::Compatible); // 1B / 12B wrap
}

TEST_CASE("Beat pins bend the grid between them")
{
    const BeatGrid steady{120.0, 1.0}; // a beat every 0.5 s from 1 s
    CHECK(steady.secondsOfBeat(4.0) == Approx(3.0));
    CHECK(steady.beatAt(3.25) == Approx(4.5));

    // The first pin keeps the downbeat where it was: only the beats between spread out.
    const BeatGrid drift = steady.pinned(100, 51.5); // beat 100 (51 s) really 0.5 s late
    REQUIRE(drift.pins.size() == 2);
    CHECK(drift.pins[0].beat == 0);
    CHECK(drift.secondsOfBeat(0.0) == Approx(1.0));
    CHECK(drift.secondsOfBeat(50.0) == Approx(26.25));
    CHECK(drift.periodAt(20.0) == Approx(0.505));
    CHECK(drift.beatAt(26.25) == Approx(50.0));
    // Outside the pins, the beats carry on at the BPM.
    CHECK(drift.secondsOfBeat(-2.0) == Approx(0.0));
    CHECK(drift.secondsOfBeat(104.0) == Approx(53.5));
    CHECK(drift.beatAt(53.5) == Approx(104.0));
    CHECK(drift.periodAt(60.0) == Approx(0.5));

    // A splice: beat 100 pinned where it is, then 101 dragged 0.1 s later. Before 100 nothing moves.
    const BeatGrid splice = steady.pinned(100, steady.secondsOfBeat(100.0)).pinned(101, 51.6);
    CHECK(splice.secondsOfBeat(99.0) == Approx(50.5));
    CHECK(splice.secondsOfBeat(102.0) == Approx(52.1));

    // Kept between the neighbours: beats at least half as long as at the BPM.
    const BeatGrid squashed = splice.pinned(101, 50.0);
    CHECK(squashed.findPin(101)->seconds == Approx(51.25));
    CHECK(squashed.valid());

    // Unpinning down to the downbeat alone goes back to a steady grid.
    const BeatGrid back = drift.unpinned(100);
    CHECK(back.pins.empty());
    CHECK(back.secondsOfBeat(4.0) == Approx(3.0));
}

TEST_CASE("Grid edits keep beat pins in place")
{
    const BeatGrid grid = BeatGrid{120.0, 1.0}.pinned(100, 51.5);
    const BeatGrid moved = grid.shifted(0.01);
    CHECK(moved.secondsOfBeat(100.0) == Approx(51.51));
    CHECK(moved.firstBeatSeconds == Approx(1.01));

    const BeatGrid renumbered = grid.withDownbeatAt(26.3); // near beat 50
    CHECK(renumbered.secondsOfBeat(0.0) == Approx(26.25));
    CHECK(renumbered.secondsOfBeat(50.0) == Approx(51.5));

    const BeatGrid doubled = grid.doubled();
    CHECK(doubled.secondsOfBeat(200.0) == Approx(51.5));
    CHECK(doubled.secondsOfBeat(1.0) == Approx(1.2525));
    CHECK(doubled.halved() == grid);

    CHECK_FALSE(BeatGrid(120.0, 0.0, {{4, 2.0}, {2, 3.0}}).valid());
}

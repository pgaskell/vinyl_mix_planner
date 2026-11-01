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

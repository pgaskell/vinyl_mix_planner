#include "core/SetFile.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <fstream>

using namespace vmp;
using Catch::Matchers::ContainsSubstring;

namespace
{
Set makeSet()
{
    Set set;
    set.name = "Friday warm-up";
    set.numDecks = 3;
    set.tracks.push_back({1, "/Users/dj/Music/Björk/Ünïcode – track.flac", "Ünïcode", 15876000, 44100.0,
                          PressedSpeed::Rpm45, BeatGrid{123.456, 0.0123456789}, MusicalKey{10, true}});
    set.tracks.push_back({2, "/Users/dj/Music/b.wav", "", 17280000, 48000.0, PressedSpeed::Unknown, {}, {}});

    Clip clip;
    clip.id = 3;
    clip.trackId = 1;
    clip.deck = 2;
    clip.startSeconds = 123.456789012345678;
    clip.inFrame = 44100;
    clip.outFrame = 15000000;
    clip.pitchSteps = -37;
    clip.pitchMoves = {{2000000, 7.5, 12}, {3000000, 0.0, -160}};
    clip.mixer = {0.8, 0.0, 0.5, 0.6125};
    set.clips.push_back(clip);

    set.nextId = 4;
    return set;
}

std::filesystem::path tempPath(const std::string& name)
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() / ("vmp_test_" + std::to_string(stamp) + "_" + name);
}
} // namespace

TEST_CASE("A set survives a JSON round trip exactly")
{
    const auto set = makeSet();
    CHECK(setFromJson(setToJson(set)) == set);
}

TEST_CASE("Saving writes the file in one step and leaves no temporary behind")
{
    const auto path = tempPath("set.vmpset");
    const auto set = makeSet();
    saveSet(set, path);
    CHECK(loadSet(path) == set);

    auto changed = set;
    changed.name = "Saturday";
    saveSet(changed, path); // overwrite
    CHECK(loadSet(path).name == "Saturday");

    auto temp = path;
    temp += ".saving";
    CHECK_FALSE(std::filesystem::exists(temp));
    std::filesystem::remove(path);
}

TEST_CASE("Missing files and bad content raise SetFileError")
{
    CHECK_THROWS_AS(loadSet(tempPath("does_not_exist.vmpset")), SetFileError);
    CHECK_THROWS_WITH(setFromJson("{ not json"), ContainsSubstring("not valid JSON"));
    CHECK_THROWS_WITH(setFromJson(R"({"format": "something-else"})"), ContainsSubstring("not a Vinyl Mix Planner set"));
    CHECK_THROWS_WITH(setFromJson(R"({"format": "vinyl-mix-planner-set", "version": 1})"), ContainsSubstring("malformed"));
}

TEST_CASE("Files from a newer version are refused rather than half-read")
{
    auto text = setToJson(makeSet());
    const auto at = text.find("\"version\": 1");
    REQUIRE(at != std::string::npos);
    text.replace(at, 12, "\"version\": 2");
    CHECK_THROWS_WITH(setFromJson(text), ContainsSubstring("newer version"));
}

TEST_CASE("An unknown pressed speed is an error, not silently 'unknown'")
{
    auto text = setToJson(makeSet());
    const auto at = text.find("\"45\"");
    REQUIRE(at != std::string::npos);
    text.replace(at, 4, "\"78\"");
    CHECK_THROWS_WITH(setFromJson(text), ContainsSubstring("78"));
}

TEST_CASE("A stale nextId is repaired so new ids stay unique")
{
    auto set = makeSet();
    set.nextId = 1;
    CHECK(setFromJson(setToJson(set)).nextId == 4);
}

#include "core/Edits.h"
#include "core/MixRecorder.h"
#include "engine/MixerChannel.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

using namespace vmp;
using Catch::Approx;

namespace
{
constexpr double kRate = 44100.0;

// One 300 s record on deck 1, needle drop at 10 s, at 0% pitch: timeline t is source (t - 10) s.
struct OneClip
{
    Set set;
    Id clip = 0;

    OneClip()
    {
        const Id track = edit::addTrack(set, {0, "/a.flac", "A", static_cast<int64_t>(300.0 * kRate), kRate,
                                              PressedSpeed::Unknown, {}, {}});
        clip = edit::addClip(set, track, 0, 10.0);
    }

    const Clip& get() const { return *set.findClip(clip); }
    double at(Param param, double seconds) const { return valueAt(get(), param, (seconds - 10.0) * kRate); }
};
} // namespace

TEST_CASE("Thinning keeps the shape of a recorded move with few points")
{
    // A slow sweep sampled at 60 Hz, with a little hand jitter: a line is enough.
    std::vector<edit::RecordedMove> sweep;
    for (int i = 0; i <= 120; ++i)
        sweep.push_back({i / 60.0, i / 120.0 + ((i % 2) != 0 ? 0.003 : -0.003)});
    const auto line = edit::thinMoves(sweep, 0.01);
    CHECK(line.size() == 2);
    CHECK(line.front().seconds == sweep.front().seconds);
    CHECK(line.back().seconds == sweep.back().seconds);

    // A fader slammed shut, then held: the corners stay.
    std::vector<edit::RecordedMove> cut;
    for (int i = 0; i <= 60; ++i)
        cut.push_back({i / 60.0, i < 30 ? 1.0 : (i < 33 ? 1.0 - (i - 29) / 4.0 : 0.0)});
    const auto corners = edit::thinMoves(cut, 0.01);
    CHECK(corners.size() == 4);
    for (const auto& move : cut) // every original move is redrawn within the tolerance
    {
        auto after = std::lower_bound(corners.begin(), corners.end(), move.seconds,
                                      [](const auto& m, double s) { return m.seconds < s; });
        if (after == corners.begin() || after == corners.end())
            continue;
        const auto before = std::prev(after);
        const double t = (move.seconds - before->seconds) / (after->seconds - before->seconds);
        CHECK(std::abs(before->value + t * (after->value - before->value) - move.value) <= 0.01);
    }
}

TEST_CASE("Pulling a fader down to mix out only changes the record from then on")
{
    // The bug a tester hit: with no keypoints, the move used to set the whole clip's level.
    OneClip c;
    edit::writeRecordedPass(c.set, c.clip, Param::Volume, {{200.0, 1.0}, {204.0, 0.0}}, 250.0);
    CHECK(c.get().mixer.fader == 1.0); // the starting value stays
    CHECK(c.at(Param::Volume, 100.0) == Approx(1.0));
    CHECK(c.at(Param::Volume, 202.0) == Approx(0.5).margin(0.01));
    CHECK(c.at(Param::Volume, 240.0) == Approx(0.0)); // held after the hand left the fader
    // Playback stopped at 250 s: from there the record goes back to what it had.
    CHECK(c.at(Param::Volume, 250.0 + edit::kRecordingReturnSeconds + 1.0) == Approx(1.0));
}

TEST_CASE("A recorded pass to the end of the clip holds there, and replaces what it covers")
{
    OneClip c;
    edit::setAutomationPoint(c.set, c.clip, Param::Low, static_cast<int64_t>(100.0 * kRate), 0.0);
    edit::setAutomationPoint(c.set, c.clip, Param::Low, static_cast<int64_t>(150.0 * kRate), 1.0);
    // Low cut at 100 s (source), recorded over from timeline 120 s to past the clip's end (310 s).
    edit::writeRecordedPass(c.set, c.clip, Param::Low, {{120.0, 0.2}, {121.0, 0.5}}, 400.0);
    CHECK(c.at(Param::Low, 110.0) == Approx(0.0));    // before the pass: as it was
    CHECK(c.at(Param::Low, 300.0) == Approx(0.5));    // the 1.0 keypoint at 160 s is gone
    for (const auto& p : c.get().points(Param::Low)) // nothing added after the clip
        CHECK(p.sourceFrame <= c.get().outFrame);
}

TEST_CASE("A pass from the needle drop sets the starting value")
{
    OneClip c;
    edit::writeRecordedPass(c.set, c.clip, Param::High, {{10.0, 0.3}, {12.0, 0.3}}, 20.0);
    CHECK(c.get().mixer.high == Approx(0.3));
    CHECK(c.at(Param::High, 15.0) == Approx(0.3));
    CHECK(c.at(Param::High, 30.0) == Approx(0.5)); // back to centre after the pass
}

TEST_CASE("A recorded pass has few keypoints")
{
    OneClip c;
    std::vector<edit::RecordedMove> moves;
    for (int i = 0; i <= 600; ++i) // ten seconds of a smooth sweep at 60 Hz
        moves.push_back({50.0 + i / 60.0, 0.5 + 0.5 * std::sin(i / 600.0 * 3.14159)});
    edit::writeRecordedPass(c.set, c.clip, Param::Mid, moves, 70.0);
    CHECK(c.get().points(Param::Mid).size() < 20);
    CHECK(c.at(Param::Mid, 55.0) == Approx(1.0).margin(0.01));
}

TEST_CASE("Pitch isn't recorded")
{
    OneClip c;
    CHECK_FALSE(edit::writeRecordedPass(c.set, c.clip, Param::Pitch, {{20.0, 5.0}}, 30.0));
}

TEST_CASE("The recorder latches each control until its pass is finished")
{
    MixRecorder recorder;
    CHECK_FALSE(recorder.recording());
    recorder.move(0, 7, 100.0, Param::Volume, 1.0, 50.0);
    recorder.move(0, 7, 100.0, Param::Volume, 0.0, 51.0);
    CHECK(recorder.latched(0, Param::Volume) == 0.0);
    CHECK_FALSE(recorder.latched(0, Param::Low));
    CHECK_FALSE(recorder.latched(1, Param::Volume));

    SECTION("until its clip ends")
    {
        recorder.finishEnded(99.0);
        CHECK(recorder.takeFinished().empty());
        recorder.finishEnded(100.5);
        const auto passes = recorder.takeFinished();
        REQUIRE(passes.size() == 1);
        CHECK(passes[0].end == 100.0);
        CHECK(passes[0].moves.size() == 2);
        CHECK_FALSE(recorder.recording());
    }
    SECTION("until playback stops")
    {
        recorder.finishAll(60.0);
        const auto passes = recorder.takeFinished();
        REQUIRE(passes.size() == 1);
        CHECK(passes[0].end == 60.0);
        CHECK_FALSE(recorder.latched(0, Param::Volume));
    }
    SECTION("or the deck's control is moved on another clip")
    {
        recorder.move(0, 8, 200.0, Param::Volume, 0.7, 98.0);
        const auto passes = recorder.takeFinished();
        REQUIRE(passes.size() == 1);
        CHECK(passes[0].clip == 7);
        CHECK(passes[0].end == 98.0);
        CHECK(recorder.latched(0, Param::Volume) == 0.7);
    }
}

TEST_CASE("Moves behind the playhead are thinned while recording; recent ones are kept as they are")
{
    MixRecorder recorder;
    for (int i = 0; i <= 600; ++i) // a ten second fade at 60 Hz
        recorder.move(1, 7, 100.0, Param::Volume, 1.0 - i / 600.0, 50.0 + i / 60.0);
    recorder.settle(58.0);
    const auto* pass = recorder.pass(1, Param::Volume);
    REQUIRE(pass != nullptr);
    const auto recent = std::count_if(pass->moves.begin(), pass->moves.end(), [](const auto& m) { return m.seconds >= 58.0; });
    CHECK(recent == 121);                           // the last two seconds, untouched
    CHECK(pass->moves.size() < 121 + 5);            // the eight before, a straight line: a few points
    CHECK(pass->moves.front().seconds == 50.0);
    CHECK(recorder.latched(1, Param::Volume) == 0.0);

    recorder.settle(58.0); // again: nothing more to do
    CHECK(recorder.pass(1, Param::Volume)->moves.size() == pass->moves.size());
    recorder.settle(61.0);
    CHECK(recorder.pass(1, Param::Volume)->moves.size() < 10);
    CHECK(recorder.pass(1, Param::Volume)->moves.back().seconds == 60.0);
}

TEST_CASE("Trimming keeps a closed fader closed, a kill a kill and COLOR off")
{
    using MC = MixerChannel;
    for (double amount : {-0.5, -0.1, 0.1, 0.5})
    {
        INFO(amount);
        CHECK(MC::trimmed(Param::Volume, 0.0, amount) == 0.0);
        CHECK(MC::trimmed(Param::Low, 0.0, amount) == 0.0);
        CHECK(MC::trimmed(Param::Color, 0.5, amount) == Approx(0.5));
    }
    for (auto param : {Param::Volume, Param::High, Param::Color})
        CHECK(MC::trimmed(param, 0.3, 0.0) == Approx(0.3));

    // Volume: a quarter turn down is 6 dB quieter, wherever the fader is.
    const double before = MC::faderGain(0.8), after = MC::faderGain(MC::trimmed(Param::Volume, 0.8, -0.25));
    CHECK(20.0 * std::log10(after / before) == Approx(-6.0).margin(0.01));
    // EQ: a quarter turn is 6 dB too, from a cut, a boost or flat.
    for (double knob : {0.3, 0.5, 0.55})
    {
        INFO(knob);
        const double dB = 20.0 * std::log10(MC::eqGain(knob));
        CHECK(20.0 * std::log10(MC::eqGain(MC::trimmed(Param::High, knob, -0.25))) == Approx(dB - 6.0).margin(0.01));
    }
    CHECK(20.0 * std::log10(MC::eqGain(MC::trimmed(Param::Mid, 0.5, 0.125))) == Approx(3.0).margin(0.01));
    // Clamped to the travel.
    CHECK(MC::trimmed(Param::Volume, 1.0, 0.5) == 1.0);
    CHECK(MC::trimmed(Param::Mid, 0.9, 1.0) == Approx(1.0));
}

TEST_CASE("A Trim pass adjusts the curve that's there from where it began, then lets it be")
{
    OneClip c;
    // A fade out from full at 100 s to closed at 120 s (timeline 110..130 s).
    edit::setAutomationPoint(c.set, c.clip, Param::Volume, static_cast<int64_t>(100.0 * kRate), 1.0);
    edit::setAutomationPoint(c.set, c.clip, Param::Volume, static_cast<int64_t>(120.0 * kRate), 0.0);
    // Trimmed down a quarter turn (-6 dB) from 100 s until playback stopped at 140 s.
    REQUIRE(edit::writeTrimmedPass(c.set, c.clip, Param::Volume, {{100.0, 0.0}, {100.5, -0.25}}, 140.0,
                                   MixerChannel::trimmed));
    const double factor = std::pow(10.0, -6.0 / 40.0); // on the fader position
    CHECK(c.at(Param::Volume, 50.0) == Approx(1.0));          // before the pass
    CHECK(c.at(Param::Volume, 110.0) == Approx(factor).margin(0.01));
    CHECK(c.at(Param::Volume, 120.0) == Approx(0.5 * factor).margin(0.01)); // the fade keeps its shape
    CHECK(c.at(Param::Volume, 135.0) == Approx(0.0).margin(1e-9)); // closed stays closed
    CHECK(c.get().points(Param::Volume).size() < 12);         // thinned

    // An EQ held flat moves by the offset, then goes back after the pass.
    REQUIRE(edit::writeTrimmedPass(c.set, c.clip, Param::Low, {{50.0, 0.0}, {51.0, -0.25}}, 60.0, MixerChannel::trimmed));
    CHECK(c.at(Param::Low, 40.0) == Approx(0.5));
    CHECK(20.0 * std::log10(MixerChannel::eqGain(c.at(Param::Low, 55.0))) == Approx(-6.0).margin(0.05));
    CHECK(c.at(Param::Low, 70.0) == Approx(0.5));
}

TEST_CASE("Trimming adjusts the whole clip's curve and keeps its keypoints")
{
    OneClip c;
    // A fade out from full at 100 s to closed at 120 s (timeline 110..130 s).
    edit::setAutomationPoint(c.set, c.clip, Param::Volume, static_cast<int64_t>(100.0 * kRate), 1.0);
    edit::setAutomationPoint(c.set, c.clip, Param::Volume, static_cast<int64_t>(120.0 * kRate), 0.0);
    REQUIRE(edit::trimAutomation(c.set, c.clip, Param::Volume, -0.25, MixerChannel::trimmed)); // -6 dB
    const double factor = std::pow(10.0, -6.0 / 40.0); // on the fader position
    CHECK(c.get().mixer.fader == Approx(factor));                    // from the needle drop
    CHECK(c.at(Param::Volume, 110.0) == Approx(factor));
    CHECK(c.at(Param::Volume, 120.0) == Approx(0.5 * factor));       // the fade keeps its shape
    CHECK(c.at(Param::Volume, 135.0) == 0.0);                        // closed stays closed
    CHECK(c.get().points(Param::Volume).size() == 2);
}

TEST_CASE("Trimming an EQ held through the record moves it by the offset")
{
    OneClip c;
    REQUIRE(edit::trimAutomation(c.set, c.clip, Param::Low, -0.25, MixerChannel::trimmed)); // -6 dB
    for (double t : {10.0, 100.0, 300.0})
        CHECK(20.0 * std::log10(MixerChannel::eqGain(c.at(Param::Low, t))) == Approx(-6.0).margin(0.01));
    CHECK_FALSE(edit::trimAutomation(c.set, c.clip, Param::Pitch, 0.25, MixerChannel::trimmed));
}

TEST_CASE("The recorder keeps trim passes apart from recorded ones")
{
    MixRecorder recorder;
    recorder.move(0, 7, 100.0, Param::High, -0.1, 50.0, AutomationMode::Trim);
    CHECK(recorder.latchedTrim(0, Param::High) == -0.1);
    CHECK_FALSE(recorder.latched(0, Param::High));
    recorder.move(0, 7, 100.0, Param::High, 0.2, 51.0); // another mode ends the trim pass
    const auto passes = recorder.takeFinished();
    REQUIRE(passes.size() == 1);
    CHECK(passes[0].mode == AutomationMode::Trim);
    CHECK(recorder.latched(0, Param::High) == 0.2);
    CHECK_FALSE(recorder.latchedTrim(0, Param::High));
}

TEST_CASE("A control keeps the mode it was taken hold of in")
{
    MixRecorder recorder;
    recorder.move(0, 7, 100.0, Param::Low, 0.0, 50.0, AutomationMode::Override); // practising a bass cut
    recorder.move(0, 7, 100.0, Param::Mid, 0.4, 50.0, AutomationMode::Latch);
    // The app switches to Latch: nothing ends, so nothing heard changes.
    CHECK(recorder.takeFinished().empty());
    CHECK(recorder.pass(0, Param::Low)->mode == AutomationMode::Override);
    CHECK(recorder.latched(0, Param::Low) == 0.0);

    // Taking hold of the bass again in Latch: the practice pass is dropped there, and the new one
    // starts from where the knob already is.
    recorder.move(0, 7, 100.0, Param::Low, 0.0, 55.0, AutomationMode::Latch);
    const auto passes = recorder.takeFinished();
    REQUIRE(passes.size() == 1);
    CHECK(passes[0].mode == AutomationMode::Override);
    CHECK(recorder.pass(0, Param::Low)->mode == AutomationMode::Latch);
    CHECK(recorder.latched(0, Param::Low) == 0.0);
}

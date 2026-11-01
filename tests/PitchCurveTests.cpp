#include "core/PitchCurve.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

using namespace vmp;
using Catch::Approx;

TEST_CASE("Constant pitch maps time linearly")
{
    const PitchCurve zero;
    CHECK(zero.speedAt(10.0) == 1.0);
    CHECK(zero.sourceSecondsAt(10.0) == Approx(10.0));
    CHECK(zero.timeAtSourceSeconds(10.0) == Approx(10.0));

    const PitchCurve plus8(160);
    CHECK(plus8.sourceSecondsAt(10.0) == Approx(10.8));
    CHECK(plus8.timeAtSourceSeconds(10.8) == Approx(10.0));
}

TEST_CASE("A ramp integrates to its average speed")
{
    // Hold at 0% until 5 s into the music, then slide to +8% over 10 s.
    const PitchCurve curve(0, {{5.0, 10.0, 160}});

    CHECK(curve.speedAt(4.0) == Approx(1.0));
    CHECK(curve.speedAt(10.0) == Approx(1.04));
    CHECK(curve.speedAt(20.0) == Approx(1.08));

    CHECK(curve.sourceSecondsAt(5.0) == Approx(5.0));
    CHECK(curve.sourceSecondsAt(15.0) == Approx(5.0 + 10.0 * 1.04));
    CHECK(curve.sourceSecondsAt(25.0) == Approx(15.4 + 10.0 * 1.08));
}

TEST_CASE("A zero-length move is a jump")
{
    const PitchCurve curve(0, {{5.0, 0.0, -160}});
    CHECK(curve.speedAt(4.999) == Approx(1.0));
    CHECK(curve.speedAt(5.001) == Approx(0.92));
    CHECK(curve.sourceSecondsAt(15.0) == Approx(5.0 + 10.0 * 0.92));
}

TEST_CASE("Time and source position are exact inverses")
{
    // Base pitch, a ramp up, a jump down, a ramp down.
    const PitchCurve curve(40, {{10.0, 8.0, 160}, {30.0, 0.0, -100}, {50.0, 20.0, 0}});
    for (double t = -5.0; t < 120.0; t += 0.37)
    {
        INFO("t = " << t);
        CHECK(curve.timeAtSourceSeconds(curve.sourceSecondsAt(t)) == Approx(t).margin(1e-9));
    }
}

TEST_CASE("Moves are anchored to the music")
{
    // A move starting at 20 s of music happens later in real time when playing slower.
    const PitchCurve slow(-160, {{20.0, 0.0, 0}});
    const PitchCurve fast(160, {{20.0, 0.0, 0}});
    CHECK(slow.timeAtSourceSeconds(20.0) == Approx(20.0 / 0.92));
    CHECK(fast.timeAtSourceSeconds(20.0) == Approx(20.0 / 1.08));
}

TEST_CASE("Moves can be given in any order")
{
    const PitchCurve sorted(0, {{5.0, 2.0, 20}, {20.0, 2.0, 40}});
    const PitchCurve unsorted(0, {{20.0, 2.0, 40}, {5.0, 2.0, 20}});
    CHECK(unsorted.sourceSecondsAt(60.0) == Approx(sorted.sourceSecondsAt(60.0)));
}

TEST_CASE("Moves before the needle drop are ignored")
{
    const PitchCurve curve(0, {{-3.0, 1.0, 160}});
    CHECK(curve.speedAt(10.0) == 1.0);
}

TEST_CASE("Invalid moves are rejected")
{
    // The first ramp ends at 5 + 10 * 1.04 = 15.4 s of music.
    CHECK_THROWS_AS(PitchCurve(0, {{5.0, 10.0, 160}, {10.0, 1.0, 0}}), std::invalid_argument);
    CHECK_NOTHROW(PitchCurve(0, {{5.0, 10.0, 160}, {15.4, 1.0, 0}}));
    CHECK_THROWS_AS(PitchCurve(0, {{5.0, -1.0, 160}}), std::invalid_argument);
}

TEST_CASE("Before the needle drop the curve extrapolates at the starting speed")
{
    const PitchCurve curve(80);
    CHECK(curve.sourceSecondsAt(-2.0) == Approx(-2.08));
    CHECK(curve.timeAtSourceSeconds(-2.08) == Approx(-2.0));
}

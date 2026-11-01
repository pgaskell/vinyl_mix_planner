#include "core/PitchCurve.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace vmp;
using Catch::Approx;

TEST_CASE("Constant pitch maps time linearly")
{
    const PitchCurve zero;
    CHECK(zero.speedAt(10.0) == 1.0);
    CHECK(zero.sourceSecondsAt(10.0) == Approx(10.0));
    CHECK(zero.timeAtSourceSeconds(10.0) == Approx(10.0));

    const PitchCurve plus8(1.08);
    CHECK(plus8.sourceSecondsAt(10.0) == Approx(10.8));
    CHECK(plus8.timeAtSourceSeconds(10.8) == Approx(10.0));
}

TEST_CASE("A slide between points integrates to its average speed")
{
    // Hold at 0% until 5 s into the music, then slide to +8%, arriving at 15.4 s of music,
    // which takes 10 s at an average speed of 1.04.
    const PitchCurve curve(1.0, {{5.0, 1.0}, {15.4, 1.08}});

    CHECK(curve.speedAt(4.0) == Approx(1.0));
    CHECK(curve.speedAt(10.0) == Approx(1.04));
    CHECK(curve.speedAt(20.0) == Approx(1.08));

    CHECK(curve.sourceSecondsAt(5.0) == Approx(5.0));
    CHECK(curve.sourceSecondsAt(15.0) == Approx(15.4));
    CHECK(curve.sourceSecondsAt(25.0) == Approx(15.4 + 10.0 * 1.08));
}

TEST_CASE("Points set the speed before the first and after the last")
{
    const PitchCurve curve(1.0, {{10.0, 1.02}, {20.0, 1.02}});
    CHECK(curve.speedAt(0.0) == Approx(1.02)); // the base speed is not used once there are points
    CHECK(curve.speedAt(100.0) == Approx(1.02));
}

TEST_CASE("Two points at one position make a jump")
{
    const PitchCurve curve(1.0, {{5.0, 1.0}, {5.0, 0.92}});
    CHECK(curve.speedAt(4.999) == Approx(1.0));
    CHECK(curve.speedAt(5.001) == Approx(0.92));
    CHECK(curve.sourceSecondsAt(15.0) == Approx(5.0 + 10.0 * 0.92));
}

TEST_CASE("Time and source position are exact inverses")
{
    const PitchCurve curve(1.0, {{10.0, 1.02}, {18.0, 1.08}, {30.0, 1.08}, {30.0, 0.95}, {50.0, 0.95}, {70.0, 1.0}});
    for (double t = -5.0; t < 120.0; t += 0.37)
    {
        INFO("t = " << t);
        CHECK(curve.timeAtSourceSeconds(curve.sourceSecondsAt(t)) == Approx(t).margin(1e-9));
    }
}

TEST_CASE("Points are anchored to the music")
{
    // A slide starting 20 s into the music happens later in real time when playing slower.
    const PitchCurve slow(1.0, {{0.0, 0.92}, {20.0, 0.92}, {20.0, 1.0}});
    const PitchCurve fast(1.0, {{0.0, 1.08}, {20.0, 1.08}, {20.0, 1.0}});
    CHECK(slow.timeAtSourceSeconds(20.0) == Approx(20.0 / 0.92));
    CHECK(fast.timeAtSourceSeconds(20.0) == Approx(20.0 / 1.08));
}

TEST_CASE("Points can be given in any order")
{
    const PitchCurve sorted(1.0, {{5.0, 1.0}, {7.0, 1.01}, {20.0, 1.02}});
    const PitchCurve unsorted(1.0, {{20.0, 1.02}, {5.0, 1.0}, {7.0, 1.01}});
    CHECK(unsorted.sourceSecondsAt(60.0) == Approx(sorted.sourceSecondsAt(60.0)));
}

TEST_CASE("A needle drop part way through a slide starts at the speed the slide had reached")
{
    // The full slide: 1.0 at -10 s of music to 1.08 at +10.8 s.
    const PitchCurve whole(1.0, {{0.0, 1.0}, {20.8, 1.08}});
    const PitchCurve trimmed(1.0, {{-10.0, 1.0}, {10.8, 1.08}});
    const double t = whole.timeAtSourceSeconds(10.0);
    CHECK(trimmed.speedAt(0.0) == Approx(whole.speedAt(t)));
    // ...and continues it exactly.
    CHECK(trimmed.sourceSecondsAt(3.0) == Approx(whole.sourceSecondsAt(t + 3.0) - 10.0));

    // All points behind the needle drop: the last speed holds.
    CHECK(PitchCurve(1.0, {{-5.0, 1.03}}).speedAt(1.0) == Approx(1.03));
}

TEST_CASE("Before the needle drop the curve extrapolates at the starting speed")
{
    const PitchCurve curve(1.04);
    CHECK(curve.sourceSecondsAt(-2.0) == Approx(-2.08));
    CHECK(curve.timeAtSourceSeconds(-2.08) == Approx(-2.0));
}

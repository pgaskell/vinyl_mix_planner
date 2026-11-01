#include "core/Pitch.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace vmp;
using Catch::Approx;

TEST_CASE("Pitch percent snaps to the 0.02% grid")
{
    CHECK(pitchStepsFromPercent(0.0) == 0);
    CHECK(pitchStepsFromPercent(1.24) == 62);
    CHECK(pitchStepsFromPercent(1.234) == 62);
    CHECK(pitchStepsFromPercent(1.224) == 61);
    CHECK(pitchStepsFromPercent(-3.1) == -155);
    CHECK(pitchPercentFromSteps(62) == Approx(1.24));
}

TEST_CASE("Pitch is clamped to the MK2's ±8% range")
{
    CHECK(pitchStepsFromPercent(8.0) == 400);
    CHECK(pitchStepsFromPercent(9.5) == 400);
    CHECK(pitchStepsFromPercent(-12.0) == -400);
    CHECK(pitchPercentFromSteps(500) == Approx(8.0));
}

TEST_CASE("Speed and key move together")
{
    CHECK(speedFromPitchSteps(0) == Approx(1.0));
    CHECK(speedFromPitchSteps(400) == Approx(1.08));
    CHECK(speedFromPitchSteps(-400) == Approx(0.92));
    CHECK(semitoneShiftForSpeed(1.0) == Approx(0.0));
    CHECK(semitoneShiftForSpeed(1.06) == Approx(1.0088).epsilon(1e-3));
    CHECK(semitoneShiftForSpeed(2.0) == Approx(12.0));
}

#include "core/Motor.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace vmp;
using Catch::Approx;

TEST_CASE("Motor ramps up over the start time and down over the stop time")
{
    MotorModel motor;
    motor.setTimes(0.7, 0.5);
    motor.setRunning(true);

    CHECK(motor.advance(24000, 48000.0) == Approx(0.5 / 0.7));
    CHECK(motor.advance(48000, 48000.0) == Approx(1.0)); // clamps at full speed

    motor.setRunning(false);
    CHECK(motor.advance(12000, 48000.0) == Approx(0.5));
    CHECK(motor.advance(48000, 48000.0) == Approx(0.0));
}

TEST_CASE("Zero start/stop time switches instantly")
{
    MotorModel motor;
    motor.setTimes(0.0, 0.0);
    motor.setRunning(true);
    CHECK(motor.advance(1, 48000.0) == 1.0);
    motor.setRunning(false);
    CHECK(motor.advance(1, 48000.0) == 0.0);
}

TEST_CASE("settle() jumps to the target speed")
{
    MotorModel motor;
    motor.setRunning(true);
    motor.settle();
    CHECK(motor.fraction() == 1.0);
}

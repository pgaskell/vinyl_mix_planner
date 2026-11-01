#pragma once

#include <algorithm>

namespace vmp
{

// Technics spec: 0.7 s to reach 33 1/3 rpm from standstill.
inline constexpr double kMk2StartSeconds = 0.7;
// The MK2 brake is not adjustable and not in the spec sheet. Estimate; measure on a real deck.
inline constexpr double kMk2StopSeconds = 0.5;

// Platter speed as a fraction of nominal (0 = stopped, 1 = at speed). Linear ramps for now;
// the real start curve is closer to exponential, which matters only for start/stop effects.
class MotorModel
{
public:
    void setTimes(double startSeconds, double stopSeconds)
    {
        startSeconds_ = std::max(0.0, startSeconds);
        stopSeconds_ = std::max(0.0, stopSeconds);
    }

    void setRunning(bool running) { running_ = running; }
    bool isRunning() const { return running_; }

    // Jump straight to the target speed, e.g. when loading a deck that is already "spinning".
    void settle() { fraction_ = running_ ? 1.0 : 0.0; }

    // Advance by a number of frames and return the speed fraction at the end of that span.
    double advance(int frames, double sampleRate)
    {
        const double seconds = frames / sampleRate;
        if (running_)
            fraction_ = startSeconds_ <= 0.0 ? 1.0 : std::min(1.0, fraction_ + seconds / startSeconds_);
        else
            fraction_ = stopSeconds_ <= 0.0 ? 0.0 : std::max(0.0, fraction_ - seconds / stopSeconds_);
        return fraction_;
    }

    double fraction() const { return fraction_; }

private:
    double startSeconds_ = kMk2StartSeconds;
    double stopSeconds_ = kMk2StopSeconds;
    double fraction_ = 0.0;
    bool running_ = false;
};

} // namespace vmp

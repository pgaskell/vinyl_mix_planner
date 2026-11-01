#pragma once

#include "engine/Engine.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

namespace vmp
{

// A vertical stereo peak meter (left and right bars) from -48 to +6 dBFS: the bars fall back at
// 24 dB/s, a line holds each side's highest level for 1.5 s, and each side's clip light stays lit
// after it reaches full scale (0 dBFS) until clicked.
class LevelMeter : public juce::Component, public juce::SettableTooltipClient
{
public:
    LevelMeter();

    // From the UI timer: the peaks since the last call, and the time since then.
    void push(StereoPeak peak, double elapsedSeconds);
    // Greyed out when there's nothing to measure (e.g. the internal mix in direct out).
    void setActive(bool active);

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent&) override;

private:
    struct Side
    {
        float levelDb = -100.0f;
        float holdDb = -100.0f;
        double holdAge = 0.0;
        bool clipped = false;

        bool push(float peak, double elapsedSeconds); // true if it needs repainting
    };
    void paintSide(juce::Graphics& g, const Side& side, juce::Rectangle<float> area) const;

    std::array<Side, 2> sides_;
    bool active_ = true;
};

} // namespace vmp

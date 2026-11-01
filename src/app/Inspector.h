#pragma once

#include "core/Document.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace vmp
{

// Settings of the selected clip as you'd set them before bringing the record in: pitch fader
// (+ at the bottom, like a 1200), channel fader and EQ.
class Inspector : public juce::Component
{
public:
    explicit Inspector(SetDocument& document);

    void setClip(Id clipId);
    // The set changed.
    void refresh();

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void setUpSlider(juce::Slider& slider, juce::Label& label, const juce::String& name);
    void applyPitch(int steps);
    void applyMixer();

    SetDocument& document_;
    Id clip_ = 0;

    juce::Label title_, details_;
    juce::Slider pitch_, fader_, high_, mid_, low_;
    juce::Label pitchLabel_, faderLabel_, highLabel_, midLabel_, lowLabel_;
    juce::Label keyShift_;
    juce::TextButton pitchUp_{"+0.05"}, pitchDown_{"-0.05"}, pitchZero_{"0"};
};

} // namespace vmp

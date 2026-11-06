#pragma once

#include "core/Document.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace vmp
{

// Controls with automation keypoints are drawn in this colour.
inline const juce::Colour kAutomatedColour{0xffffb340};

// The clip a deck's controls (mixer strip, pitch fader) work on. Stopped: the selected clip if it
// is on that deck, otherwise the one under the playhead. Playing: the one under the playhead
// comes first, so the controls move the record being heard. 0 if none.
Id clipForDeck(const SetDocument& document, int deck, Id selectedClip, double playhead, bool playing);

// Vertical faders drawn with a wide flat cap on a thin groove, like a mixer's, instead of a dot.
class FaderLookAndFeel : public juce::LookAndFeel_V4
{
public:
    static FaderLookAndFeel& instance();

    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos, float minSliderPos,
                          float maxSliderPos, juce::Slider::SliderStyle style, juce::Slider& slider) override;
    int getSliderThumbRadius(juce::Slider& slider) override;
};

// A deck's pitch fader, + at the bottom like a 1200's, in kPitchStepPercent steps. Shows the pitch
// at the playhead of the clip it's given; moving it changes that clip's pitch (or its keypoint at
// the playhead), and the records mixed in after it follow (edit::withFollowers).
class PitchFader : public juce::Component
{
public:
    PitchFader(int deck, SetDocument& document, std::function<double()> playhead);
    ~PitchFader() override;

    void show(Id clip, double playhead);
    void resized() override;

private:
    SetDocument& document_;
    std::function<double()> playhead_;
    Id clip_ = 0;
    juce::Label label_;
    juce::Slider slider_;
};

} // namespace vmp

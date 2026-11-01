#pragma once

#include "app/AudioPool.h"
#include "app/Display.h"
#include "core/Document.h"

#include <juce_gui_basics/juce_gui_basics.h>


namespace vmp
{

// About the selected clip: tempo and key as heard after pitch, how its key sits with the track
// it's mixed with, beat-grid fixes and nudges. (Its faders and knobs are in the mixer panel.)
class Inspector : public juce::Component
{
public:
    Inspector(SetDocument& document, AudioPool& pool, const DisplayOptions& display,
              juce::ApplicationCommandManager& commands);

    void setClip(Id clipId);
    // The set, the loaded audio or the display options changed.
    void refresh();
    // Key mappings changed: update the shortcuts shown in button tooltips.
    void refreshShortcuts();

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void setUpCommandButton(juce::TextButton& button, juce::CommandID command, const juce::String& tooltip);
    void applyBpm(const juce::String& text);
    void applyKey();
    juce::String harmonyText(const Clip& clip, const Track& track, juce::Colour& colour) const;

    SetDocument& document_;
    AudioPool& pool_;
    const DisplayOptions& display_;
    juce::ApplicationCommandManager& commands_;
    Id clip_ = 0;
    struct CommandButton
    {
        juce::TextButton* button;
        juce::CommandID command;
        juce::String tooltip;
    };
    std::vector<CommandButton> commandButtons_;

    juce::Label title_, details_;

    juce::Label tempoCaption_, tempo_, bpm_;
    juce::TextButton halve_{"/2"}, double_{"x2"}, gridEarlier_{"<"}, gridLater_{">"}, downbeat_{"Downbeat here"},
        gridReset_{"Reset"};

    juce::Label keyCaption_, key_, harmony_;
    juce::ComboBox keyBox_;

    juce::Label nudgeCaption_;
    juce::TextButton beatBack_{"-beat"}, back10_{"-10"}, back1_{"-1"}, forward1_{"+1"}, forward10_{"+10"},
        beatForward_{"+beat"};

    juce::Label pitchCaption_;
    juce::TextButton pitchDown_{"-0.05"}, pitchZero_{"0"}, pitchUp_{"+0.05"};
};

} // namespace vmp

#pragma once

#include "app/AudioPool.h"
#include "app/Display.h"
#include "core/Document.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>

namespace vmp
{

// One deck's controls: pitch fader (+ at the bottom, like a 1200), Hi/Mid/Low and volume.
// They show the values at the playhead, so automation moves them during playback. Turning one
// sets the clip's starting value, or, once that control has keypoints, writes one at the
// playhead. Controls with keypoints are drawn in their automation colour.
class ChannelStrip : public juce::Component
{
public:
    ChannelStrip(int deck, SetDocument& document, std::function<double()> playhead);

    // Shows `clip` (0 for none) with its values at `playhead`.
    void show(Id clip, double playhead, const juce::String& title, const juce::String& key);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void setUpControl(juce::Slider& slider, juce::Label& label, Param param);
    void apply(Param param, double value);

    SetDocument& document_;
    std::function<double()> playhead_;
    Id clip_ = 0;

    juce::Label deckLabel_, title_, key_;
    juce::Slider pitch_, volume_, high_, mid_, low_;
    juce::Label pitchLabel_, volumeLabel_, highLabel_, midLabel_, lowLabel_;
};

class MixerPanel : public juce::Component
{
public:
    MixerPanel(SetDocument& document, AudioPool& pool, const DisplayOptions& display,
               std::function<double()> playhead);

    // Picks each deck's clip, the selected clip if it is on that deck, otherwise the one under the
    // playhead, and refreshes the strips. Called from the UI timer.
    void update(Id selectedClip, double playhead);

    // Width needed for this many decks.
    static int preferredWidth(int numDecks);

    void resized() override;

private:
    SetDocument& document_;
    AudioPool& pool_;
    const DisplayOptions& display_;
    std::array<std::unique_ptr<ChannelStrip>, kMaxDecks> strips_;
    int shownDecks_ = 0;
};

} // namespace vmp

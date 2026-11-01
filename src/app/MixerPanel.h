#pragma once

#include "app/AudioPool.h"
#include "app/Display.h"
#include "app/LevelMeter.h"
#include "core/Document.h"
#include "core/HardwareMixer.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>
#include <optional>

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
    LevelMeter& meter() { return meter_; }

    // Where this deck's audio goes on a hardware mixer (direct out) and that channel's control
    // positions, drawn as markers beside ours.
    struct HardwareView
    {
        juce::String route, tooltip;
        bool warning = false; // the deck can't be heard on the mixer
        std::optional<double> fader, high, mid, low;
        bool operator==(const HardwareView&) const = default;
    };
    void showHardware(const std::optional<HardwareView>& view);

    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;
    void resized() override;

private:
    void setUpControl(juce::Slider& slider, juce::Label& label, Param param);
    void apply(Param param, double value);

    SetDocument& document_;
    std::function<double()> playhead_;
    Id clip_ = 0;

    juce::Label deckLabel_, title_, key_, routeLabel_;
    std::optional<HardwareView> hardware_;
    juce::Slider pitch_, volume_, high_, mid_, low_;
    juce::Label pitchLabel_, volumeLabel_, highLabel_, midLabel_, lowLabel_;
    LevelMeter meter_;
};

class MixerPanel : public juce::Component
{
public:
    MixerPanel(SetDocument& document, AudioPool& pool, const DisplayOptions& display,
               std::function<double()> playhead);

    // Picks each deck's clip, the selected clip if it is on that deck, otherwise the one under the
    // playhead, and refreshes the strips. Called from the UI timer.
    void update(Id selectedClip, double playhead);
    // Peak levels since the last call, from the UI timer. The mix meter is greyed out when there
    // is no internal mix (direct out).
    void pushLevels(const std::array<StereoPeak, kMaxDecks>& decks, StereoPeak mix, bool mixActive,
                    double elapsedSeconds);
    // The hardware mixer's state, with each deck's output pair (-1: none), or nullptr to hide it.
    void showHardware(const HardwareMixer* mixer, const std::array<int, kMaxDecks>& deckPairs);

    // Width needed for this many decks.
    static int preferredWidth(int numDecks);

    void resized() override;

private:
    SetDocument& document_;
    AudioPool& pool_;
    const DisplayOptions& display_;
    std::array<std::unique_ptr<ChannelStrip>, kMaxDecks> strips_;
    LevelMeter mixMeter_;
    juce::Label mixLabel_;
    int shownDecks_ = 0;
};

} // namespace vmp

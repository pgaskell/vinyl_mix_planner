#pragma once

#include "app/AudioPool.h"
#include "app/Display.h"
#include "app/LevelMeter.h"
#include "core/Document.h"
#include "core/HardwareMixer.h"
#include "core/MixRecorder.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>
#include <optional>

namespace vmp
{


// While playing, moves of the automatable mixer controls (not pitch or the trim knob) go to the
// recorder instead of into the set.
struct RecordHooks
{
    // Takes a move of `param` on `deck`, whose strip shows `clip`: a position, or in Trim a
    // trim amount. False if it isn't taken (not playing, or that clip isn't the one playing): then
    // it edits the set.
    std::function<bool(int deck, Id clip, Param param, double value)> move;
    // Where a moved control is latched, if it is: a position, or a trim amount.
    std::function<std::optional<double>(int deck, Param param)> latched;
    std::function<std::optional<double>(int deck, Param param)> latchedTrim;
    // The mode taking hold of a control on `deck` uses now (MainComponent::modeFor).
    std::function<AutomationMode(int deck, Param param)> mode;
    // Whether a held control's pass will be written (taken hold of in Latch, not Override).
    std::function<bool(int deck, Param param)> writing;
};

// One deck's mixer channel: trim, Hi/Mid/Low, Color and volume. (The pitch fader is a turntable
// control: it's beside the deck's lane, PitchFader.)
// The trim is one value per clip; the others can be automated.
// They show the values at the playhead, so automation moves them during playback. While playing,
// moving one is taken over (see AutomationMode): drawn white in Override, red in Latch, yellow in Trim (where
// the popup shows the trim amount). Stopped, turning one sets the clip's starting value, or, once
// that control has keypoints, writes one at the playhead. Controls with keypoints are drawn in their
// automation colour.
class ChannelStrip : public juce::Component
{
public:
    ChannelStrip(int deck, SetDocument& document, std::function<double()> playhead, const RecordHooks& record);
    ~ChannelStrip() override;

    // Shows `clip` (0 for none) with its values at `playhead`.
    void show(Id clip, double playhead, const juce::String& title, const juce::String& key);
    LevelMeter& meter() { return meter_; }

    // Where this deck's audio goes on a hardware mixer (direct out) and that channel's control
    // positions, drawn as markers beside ours.
    struct HardwareView
    {
        juce::String route, tooltip;
        bool warning = false; // the deck can't be heard on the mixer
        std::optional<double> fader, trim, high, mid, low, color;
        bool operator==(const HardwareView&) const = default;
    };
    void showHardware(const std::optional<HardwareView>& view);

    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;
    void resized() override;

private:
    void setUpControl(juce::Slider& slider, juce::Label& label, Param param);
    void apply(Param param, double value);
    bool record(Param param, double value);
    juce::String trimText(Param param, double position) const;

    // Trim mode: where a control was and its trim amount when it was taken hold of.
    struct TrimGrab
    {
        double position, amount;
    };
    std::array<std::optional<TrimGrab>, kNumParams> trimGrab_;
    std::array<double, kNumParams> shown_{}; // positions last shown

    const int deck_;
    SetDocument& document_;
    const RecordHooks& record_;
    std::function<double()> playhead_;
    juce::Colour colour_; // the deck's
    Id clip_ = 0;

    juce::Label deckLabel_, title_, key_, routeLabel_;
    std::optional<HardwareView> hardware_;
    juce::Slider volume_, trim_, high_, mid_, low_, color_;
    juce::Label volumeLabel_, trimLabel_, highLabel_, midLabel_, lowLabel_, colorLabel_;
    LevelMeter meter_;
};

class MixerPanel : public juce::Component
{
public:
    MixerPanel(SetDocument& document, AudioPool& pool, const DisplayOptions& display,
               std::function<double()> playhead);

    // Picks each deck's clip (clipForDeck) and refreshes the strips. Called from the UI timer.
    void update(Id selectedClip, double playhead, bool playing);
    void setRecordHooks(RecordHooks hooks) { record_ = std::move(hooks); }
    // Peak levels since the last call, from the UI timer. The mix meter is greyed out when there
    // is no internal mix (direct out).
    void pushLevels(const std::array<StereoPeak, kMaxDecks>& decks, StereoPeak mix, bool mixActive,
                    double elapsedSeconds);
    // The hardware mixer's state, with each deck's output pair (-1: none), or nullptr to hide it.
    void showHardware(const HardwareMixer* mixer, const std::array<int, kMaxDecks>& deckPairs);
    // The internal mix, played from the hardware mixer's controls: channel n plays deck n.
    void showHardwareControl(const HardwareMixer& mixer);

    // Width needed for this many decks.
    static int preferredWidth(int numDecks);
    // The mix meter's column, at the right of the channel strips.
    static constexpr int kMixMeterWidth = 26;

    void resized() override;

private:
    SetDocument& document_;
    AudioPool& pool_;
    const DisplayOptions& display_;
    RecordHooks record_; // before strips_, which refer to it
    std::array<std::unique_ptr<ChannelStrip>, kMaxDecks> strips_;
    LevelMeter mixMeter_;
    juce::Label mixLabel_;
    int shownDecks_ = 0;
};

} // namespace vmp

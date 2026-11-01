#include "app/MixerPanel.h"

#include "core/Edits.h"
#include "core/Pitch.h"
#include "engine/MixerChannel.h"

namespace vmp
{

namespace
{
constexpr int kStripWidth = 92;

const juce::Colour kAutomatedColour{0xffffb340};

juce::String gainText(float gain)
{
    if (gain <= 0.0f)
        return "kill";
    return juce::String::formatted("%+.1f", 20.0 * std::log10(gain) + 0.0);
}
} // namespace

// --- ChannelStrip -----------------------------------------------------------

ChannelStrip::ChannelStrip(int deck, SetDocument& document, std::function<double()> playhead)
    : document_(document), playhead_(std::move(playhead))
{
    deckLabel_.setText("Deck " + juce::String(deck + 1), juce::dontSendNotification);
    deckLabel_.setFont(juce::FontOptions(14.0f, juce::Font::bold));
    deckLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(deckLabel_);
    for (auto* label : {&title_, &key_})
    {
        label->setFont(juce::FontOptions(11.0f));
        label->setJustificationType(juce::Justification::centred);
        label->setMinimumHorizontalScale(0.6f);
        addAndMakeVisible(label);
    }

    for (auto [knob, label, param] : {std::tuple{&high_, &highLabel_, Param::High},
                                      std::tuple{&mid_, &midLabel_, Param::Mid},
                                      std::tuple{&low_, &lowLabel_, Param::Low}})
    {
        setUpControl(*knob, *label, param);
        knob->setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        knob->setRange(0.0, 1.0);
        knob->setDoubleClickReturnValue(true, 0.5);
        knob->setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        knob->textFromValueFunction = [](double v) { return gainText(MixerChannel::eqGain(v)) + " dB"; };
        knob->setPopupDisplayEnabled(true, true, nullptr);
    }

    // + toward the DJ (bottom), as on the 1200. 0.05% steps.
    setUpControl(pitch_, pitchLabel_, Param::Pitch);
    pitch_.setSliderStyle(juce::Slider::LinearVertical);
    pitch_.setNormalisableRange(juce::NormalisableRange<double>(
        -8.0, 8.0, [](double, double, double proportion) { return 8.0 - 16.0 * proportion; },
        [](double, double, double value) { return (8.0 - value) / 16.0; },
        [](double, double, double value) { return std::round(value / kPitchStepPercent) * kPitchStepPercent; }));
    pitch_.setDoubleClickReturnValue(true, 0.0);
    pitch_.setTextBoxStyle(juce::Slider::TextBoxBelow, true, 46, 16);
    pitch_.textFromValueFunction = [](double v) { return juce::String::formatted("%+.2f", v + 0.0); };
    pitch_.valueFromTextFunction = [](const juce::String& s) { return s.retainCharacters("+-.0123456789").getDoubleValue(); };

    setUpControl(volume_, volumeLabel_, Param::Volume);
    volume_.setSliderStyle(juce::Slider::LinearVertical);
    volume_.setRange(0.0, 1.0);
    volume_.setDoubleClickReturnValue(true, 1.0);
    volume_.setTextBoxStyle(juce::Slider::TextBoxBelow, true, 40, 16);
    volume_.textFromValueFunction = [](double v) { return gainText(MixerChannel::faderGain(v)); };

    show(0, 0.0, {}, {});
}

void ChannelStrip::setUpControl(juce::Slider& slider, juce::Label& label, Param param)
{
    slider.setWantsKeyboardFocus(false);
    // A whole drag undoes as one step.
    slider.onDragStart = [this] { document_.beginGesture(); };
    slider.onDragEnd = [this] { document_.endGesture(); };
    slider.onValueChange = [this, &slider, param] {
        apply(param, param == Param::Pitch ? slider.getValue() / kPitchStepPercent : slider.getValue());
    };
    addAndMakeVisible(slider);

    label.setText(paramName(param), juce::dontSendNotification);
    label.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    label.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(label);
}

void ChannelStrip::apply(Param param, double value)
{
    const Id id = clip_;
    const double playhead = playhead_();
    document_.edit([&](Set& s) { return edit::setParamAt(s, id, param, value, playhead); });
}

void ChannelStrip::show(Id clipId, double playhead, const juce::String& title, const juce::String& key)
{
    clip_ = clipId;
    const Set& set = document_.set();
    const Clip* clip = set.findClip(clipId);
    const Track* track = clip != nullptr ? set.findTrack(clip->trackId) : nullptr;
    for (auto* c : std::initializer_list<juce::Component*>{&pitch_, &volume_, &high_, &mid_, &low_})
        c->setEnabled(track != nullptr);
    title_.setText(track != nullptr ? title : "-", juce::dontSendNotification);
    key_.setText(key, juce::dontSendNotification);
    if (track == nullptr)
    {
        // Nothing on this deck here: neutral and greyed out, not the last clip's values.
        for (auto* slider : {&pitch_, &volume_, &high_, &mid_, &low_})
        {
            slider->setColour(juce::Slider::thumbColourId, getLookAndFeel().findColour(juce::Slider::thumbColourId));
            slider->setColour(juce::Slider::rotarySliderFillColourId,
                              getLookAndFeel().findColour(juce::Slider::rotarySliderFillColourId));
        }
        pitch_.setValue(0.0, juce::dontSendNotification);
        volume_.setValue(1.0, juce::dontSendNotification);
        for (auto* knob : {&high_, &mid_, &low_})
            knob->setValue(0.5, juce::dontSendNotification);
        return;
    }

    // Values at the playhead, or at the nearer end of the clip when the playhead is outside it.
    const ClipTiming timing(*clip, *track);
    const double t = std::clamp(playhead, timing.startSeconds(), timing.endSeconds());
    const double frame = timing.sourceFrameAt(t);

    auto showValue = [&](juce::Slider& slider, Param param, double value) {
        const bool automated = !clip->points(param).empty();
        slider.setColour(juce::Slider::thumbColourId,
                         automated ? kAutomatedColour : getLookAndFeel().findColour(juce::Slider::thumbColourId));
        slider.setColour(juce::Slider::rotarySliderFillColourId,
                         automated ? kAutomatedColour
                                   : getLookAndFeel().findColour(juce::Slider::rotarySliderFillColourId));
        if (!slider.isMouseButtonDown()) // don't fight the user's drag
            slider.setValue(value, juce::dontSendNotification);
    };
    showValue(pitch_, Param::Pitch, (timing.speedAt(t) - 1.0) * 100.0);
    showValue(volume_, Param::Volume, valueAt(*clip, Param::Volume, frame));
    showValue(high_, Param::High, valueAt(*clip, Param::High, frame));
    showValue(mid_, Param::Mid, valueAt(*clip, Param::Mid, frame));
    showValue(low_, Param::Low, valueAt(*clip, Param::Low, frame));
}

void ChannelStrip::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff23262b));
    g.setColour(juce::Colour(0xff3a3e45));
    g.drawRect(getLocalBounds());
}

void ChannelStrip::resized()
{
    auto area = getLocalBounds().reduced(4);
    deckLabel_.setBounds(area.removeFromTop(18));
    title_.setBounds(area.removeFromTop(16));
    key_.setBounds(area.removeFromTop(16));
    area.removeFromTop(4);

    const int knobHeight = std::min(60, area.getHeight() / 7);
    for (auto [knob, label] : {std::pair{&high_, &highLabel_}, std::pair{&mid_, &midLabel_}, std::pair{&low_, &lowLabel_}})
    {
        label->setBounds(area.removeFromTop(14));
        knob->setBounds(area.removeFromTop(knobHeight - 14));
    }
    area.removeFromTop(6);

    auto labels = area.removeFromTop(14);
    auto pitchColumn = area.removeFromLeft(area.getWidth() / 2);
    pitchLabel_.setBounds(labels.removeFromLeft(labels.getWidth() / 2));
    volumeLabel_.setBounds(labels);
    pitch_.setBounds(pitchColumn);
    volume_.setBounds(area);
}

// --- MixerPanel -------------------------------------------------------------

MixerPanel::MixerPanel(SetDocument& document, AudioPool& pool, const DisplayOptions& display,
                       std::function<double()> playhead)
    : document_(document), pool_(pool), display_(display)
{
    for (int d = 0; d < kMaxDecks; ++d)
    {
        strips_[static_cast<size_t>(d)] = std::make_unique<ChannelStrip>(d, document, playhead);
        addChildComponent(*strips_[static_cast<size_t>(d)]);
    }
}

int MixerPanel::preferredWidth(int numDecks)
{
    return kStripWidth * numDecks;
}

void MixerPanel::update(Id selectedClip, double playhead)
{
    const Set& set = document_.set();
    if (set.numDecks != shownDecks_)
    {
        shownDecks_ = set.numDecks;
        resized();
    }

    const Clip* selected = set.findClip(selectedClip);
    for (int d = 0; d < set.numDecks; ++d)
    {
        const Clip* chosen = selected != nullptr && selected->deck == d ? selected : nullptr;
        if (chosen == nullptr)
            for (const auto& clip : set.clips)
                if (const Track* track = set.findTrack(clip.trackId); clip.deck == d && track != nullptr)
                {
                    const ClipTiming timing(clip, *track);
                    if (playhead >= timing.startSeconds() && playhead < timing.endSeconds())
                        chosen = &clip;
                }

        juce::String title, key;
        if (chosen != nullptr)
        {
            const Track& track = *set.findTrack(chosen->trackId);
            title = juce::CharPointer_UTF8(track.title.c_str());
            if (const auto k = pool_.keyFor(track))
            {
                const ClipTiming timing(*chosen, track);
                key = pitchedKeyText(*k, timing.speedAt(std::clamp(playhead, timing.startSeconds(), timing.endSeconds())),
                                     display_.keyNotation);
            }
        }
        strips_[static_cast<size_t>(d)]->show(chosen != nullptr ? chosen->id : 0, playhead, title, key);
    }
}

void MixerPanel::resized()
{
    auto area = getLocalBounds();
    const int n = std::max(1, shownDecks_);
    const int width = area.getWidth() / n;
    for (int d = 0; d < kMaxDecks; ++d)
    {
        auto& strip = *strips_[static_cast<size_t>(d)];
        strip.setVisible(d < shownDecks_);
        if (d < shownDecks_)
            strip.setBounds(area.removeFromLeft(width).reduced(1, 0));
    }
}

} // namespace vmp

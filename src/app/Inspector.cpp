#include "app/Inspector.h"

#include "app/TimeFormat.h"
#include "core/Edits.h"
#include "core/Pitch.h"
#include "engine/MixerChannel.h"

namespace vmp
{

namespace
{
juce::String gainText(float gain)
{
    if (gain <= 0.0f)
        return "kill";
    return juce::String::formatted("%+.1f dB", 20.0 * std::log10(gain) + 0.0);
}

const char* pressedText(PressedSpeed speed)
{
    switch (speed)
    {
    case PressedSpeed::Rpm33:
        return "33";
    case PressedSpeed::Rpm45:
        return "45";
    case PressedSpeed::Unknown:
        break;
    }
    return "unknown";
}
} // namespace

Inspector::Inspector(SetDocument& document) : document_(document)
{
    title_.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    title_.setMinimumHorizontalScale(0.6f);
    addAndMakeVisible(title_);
    details_.setJustificationType(juce::Justification::topLeft);
    details_.setFont(juce::FontOptions(12.0f));
    addAndMakeVisible(details_);

    // Pitch fader: + toward the DJ (bottom), as on the 1200. 0.05% steps.
    setUpSlider(pitch_, pitchLabel_, "Pitch");
    pitch_.setSliderStyle(juce::Slider::LinearVertical);
    pitch_.setNormalisableRange(juce::NormalisableRange<double>(
        -8.0, 8.0, [](double, double, double proportion) { return 8.0 - 16.0 * proportion; },
        [](double, double, double value) { return (8.0 - value) / 16.0; },
        [](double, double, double value) { return std::round(value / kPitchStepPercent) * kPitchStepPercent; }));
    pitch_.setDoubleClickReturnValue(true, 0.0);
    pitch_.textFromValueFunction = [](double v) { return juce::String::formatted("%+.2f%%", v + 0.0); };
    pitch_.valueFromTextFunction = [](const juce::String& s) {
        return s.retainCharacters("+-.0123456789").getDoubleValue();
    };
    pitch_.setTooltip("Pitch at the needle drop. Double-click for 0 (quartz lock).");
    pitch_.onValueChange = [this] { applyPitch(pitchStepsFromPercent(pitch_.getValue())); };

    addAndMakeVisible(keyShift_);
    keyShift_.setJustificationType(juce::Justification::centred);
    keyShift_.setFont(juce::FontOptions(12.0f));

    for (auto* button : {&pitchUp_, &pitchDown_, &pitchZero_})
    {
        button->setWantsKeyboardFocus(false);
        addAndMakeVisible(button);
    }
    pitchUp_.onClick = [this] {
        if (const Clip* clip = document_.set().findClip(clip_))
            applyPitch(clip->pitchSteps + 1);
    };
    pitchDown_.onClick = [this] {
        if (const Clip* clip = document_.set().findClip(clip_))
            applyPitch(clip->pitchSteps - 1);
    };
    pitchZero_.onClick = [this] { applyPitch(0); };

    setUpSlider(fader_, faderLabel_, "Fader");
    fader_.setSliderStyle(juce::Slider::LinearVertical);
    fader_.setRange(0.0, 1.0);
    fader_.setDoubleClickReturnValue(true, 1.0);
    fader_.textFromValueFunction = [](double v) { return gainText(MixerChannel::faderGain(v)); };

    for (auto [knob, label, name] : {std::tuple{&high_, &highLabel_, "Hi"}, std::tuple{&mid_, &midLabel_, "Mid"},
                                     std::tuple{&low_, &lowLabel_, "Low"}})
    {
        setUpSlider(*knob, *label, name);
        knob->setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        knob->setRange(0.0, 1.0);
        knob->setDoubleClickReturnValue(true, 0.5);
        knob->textFromValueFunction = [](double v) { return gainText(MixerChannel::eqGain(v)); };
    }

    for (auto* slider : {&fader_, &high_, &mid_, &low_})
        slider->onValueChange = [this] { applyMixer(); };

    refresh();
}

void Inspector::setUpSlider(juce::Slider& slider, juce::Label& label, const juce::String& name)
{
    slider.setWantsKeyboardFocus(false);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, true, 70, 18);
    // A whole drag undoes as one step.
    slider.onDragStart = [this] { document_.beginGesture(); };
    slider.onDragEnd = [this] { document_.endGesture(); };
    addAndMakeVisible(slider);

    label.setText(name, juce::dontSendNotification);
    label.setJustificationType(juce::Justification::centred);
    label.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    addAndMakeVisible(label);
}

void Inspector::setClip(Id clipId)
{
    clip_ = clipId;
    refresh();
}

void Inspector::refresh()
{
    const Set& set = document_.set();
    const Clip* clip = set.findClip(clip_);
    const Track* track = clip != nullptr ? set.findTrack(clip->trackId) : nullptr;
    const bool enabled = track != nullptr;

    for (juce::Component* c : std::initializer_list<juce::Component*>{&pitch_, &fader_, &high_, &mid_, &low_,
                                                                      &pitchUp_, &pitchDown_, &pitchZero_})
        c->setEnabled(enabled);

    if (!enabled)
    {
        title_.setText("No clip selected", juce::dontSendNotification);
        details_.setText("Drop audio files on a deck lane, or double-click a file in the library.",
                         juce::dontSendNotification);
        keyShift_.setText({}, juce::dontSendNotification);
        return;
    }

    title_.setText(juce::CharPointer_UTF8(track->title.c_str()), juce::dontSendNotification);
    juce::String details = "Deck " + juce::String(clip->deck + 1) + "   pressed at " + pressedText(track->pressed);
    try
    {
        const ClipTiming timing(*clip, *track);
        details << "\nStarts " << formatTime(timing.startSeconds(), true) << ", ends "
                << formatTime(timing.endSeconds(), true)
                << "\nRecord from " << formatTime(static_cast<double>(clip->inFrame) / track->sampleRate, true)
                << " to " << formatTime(static_cast<double>(clip->outFrame) / track->sampleRate, true);
    }
    catch (const std::invalid_argument& e)
    {
        details << "\n" << e.what();
    }
    if (!clip->pitchMoves.empty())
        details << "\n" << static_cast<int>(clip->pitchMoves.size()) << " pitch move(s)";
    details_.setText(details, juce::dontSendNotification);

    pitch_.setValue(pitchPercentFromSteps(clip->pitchSteps), juce::dontSendNotification);
    keyShift_.setText(juce::String::formatted("key %+.2f st",
                                              semitoneShiftForSpeed(speedFromPitchSteps(clip->pitchSteps)) + 0.0),
                      juce::dontSendNotification);
    fader_.setValue(clip->mixer.fader, juce::dontSendNotification);
    high_.setValue(clip->mixer.high, juce::dontSendNotification);
    mid_.setValue(clip->mixer.mid, juce::dontSendNotification);
    low_.setValue(clip->mixer.low, juce::dontSendNotification);
}

void Inspector::applyPitch(int steps)
{
    const Id id = clip_;
    document_.edit([&](Set& s) { return edit::setClipPitch(s, id, steps); });
}

void Inspector::applyMixer()
{
    const Id id = clip_;
    const ChannelSettings settings{fader_.getValue(), high_.getValue(), mid_.getValue(), low_.getValue()};
    document_.edit([&](Set& s) { return edit::setClipMixer(s, id, settings); });
}

void Inspector::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff23262b));
}

void Inspector::resized()
{
    auto area = getLocalBounds().reduced(10);
    title_.setBounds(area.removeFromTop(24));
    details_.setBounds(area.removeFromTop(76));
    area.removeFromTop(8);

    // Pitch column on the left, channel strip on the right.
    auto pitchColumn = area.removeFromLeft(area.getWidth() / 2);
    pitchLabel_.setBounds(pitchColumn.removeFromTop(18));
    auto buttons = pitchColumn.removeFromBottom(24);
    const int third = buttons.getWidth() / 3;
    pitchDown_.setBounds(buttons.removeFromLeft(third).reduced(1, 0));
    pitchZero_.setBounds(buttons.removeFromLeft(third).reduced(1, 0));
    pitchUp_.setBounds(buttons.reduced(1, 0));
    keyShift_.setBounds(pitchColumn.removeFromBottom(20));
    pitch_.setBounds(pitchColumn);

    auto strip = area;
    const int knobHeight = std::min(84, strip.getHeight() / 5);
    for (auto [knob, label] : {std::pair{&high_, &highLabel_}, std::pair{&mid_, &midLabel_}, std::pair{&low_, &lowLabel_}})
    {
        auto row = strip.removeFromTop(knobHeight);
        label->setBounds(row.removeFromTop(16));
        knob->setBounds(row);
    }
    faderLabel_.setBounds(strip.removeFromTop(18));
    fader_.setBounds(strip);
}

} // namespace vmp

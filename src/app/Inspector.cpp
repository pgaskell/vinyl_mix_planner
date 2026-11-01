#include "app/Inspector.h"

#include "app/Commands.h"
#include "app/TimeFormat.h"
#include "core/Edits.h"
#include "core/Pitch.h"
#include "engine/MixerChannel.h"

namespace vmp
{

namespace
{
constexpr int kDetectedKeyId = 1; // key menu ids: 1 = detected, then 2 + tonic * 2 + minor

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

void setCaption(juce::Label& label, const juce::String& text)
{
    label.setText(text, juce::dontSendNotification);
    label.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    label.setColour(juce::Label::textColourId, juce::Colours::grey);
}
} // namespace

Inspector::Inspector(SetDocument& document, AudioPool& pool, const DisplayOptions& display,
                     juce::ApplicationCommandManager& commands, std::function<double()> playhead)
    : document_(document), pool_(pool), display_(display), commands_(commands), playhead_(std::move(playhead))
{
    title_.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    title_.setMinimumHorizontalScale(0.6f);
    addAndMakeVisible(title_);
    for (auto* label : {&details_, &tempo_, &key_, &harmony_})
    {
        label->setJustificationType(juce::Justification::topLeft);
        label->setFont(juce::FontOptions(12.0f));
        label->setMinimumHorizontalScale(0.7f);
        addAndMakeVisible(label);
    }

    // --- Tempo ---
    setCaption(tempoCaption_, "TEMPO");
    addAndMakeVisible(tempoCaption_);
    bpm_.setEditable(false, true);
    bpm_.setTooltip("Original BPM of the record. Double-click to type a correction.");
    bpm_.setColour(juce::Label::outlineColourId, juce::Colours::grey.withAlpha(0.4f));
    bpm_.setJustificationType(juce::Justification::centred);
    bpm_.onTextChange = [this] { applyBpm(bpm_.getText()); };
    addAndMakeVisible(bpm_);
    setUpCommandButton(halve_, kGridHalve, "Halve the BPM");
    setUpCommandButton(double_, kGridDouble, "Double the BPM");
    setUpCommandButton(gridEarlier_, kGridEarlier, "Move the beat grid 2 ms earlier");
    setUpCommandButton(gridLater_, kGridLater, "Move the beat grid 2 ms later");
    setUpCommandButton(downbeat_, kGridDownbeatHere, "Put a downbeat (bar line) at the playhead");
    setUpCommandButton(gridReset_, kGridReset, "Go back to the detected tempo and grid");

    // --- Key ---
    setCaption(keyCaption_, "KEY");
    addAndMakeVisible(keyCaption_);
    keyBox_.setWantsKeyboardFocus(false);
    keyBox_.setTooltip("The record's key. Correct it here if the detection is wrong.");
    keyBox_.onChange = [this] { applyKey(); };
    addAndMakeVisible(keyBox_);

    // --- Nudge ---
    setCaption(nudgeCaption_, "NUDGE (ms)");
    addAndMakeVisible(nudgeCaption_);
    setUpCommandButton(beatBack_, kNudgeBeatEarlier, "Slip the record one beat earlier");
    setUpCommandButton(back10_, kNudgeEarlier, "Slip the record 10 ms earlier");
    setUpCommandButton(back1_, kNudgeEarlierFine, "Slip the record 1 ms earlier");
    setUpCommandButton(forward1_, kNudgeLaterFine, "Slip the record 1 ms later");
    setUpCommandButton(forward10_, kNudgeLater, "Slip the record 10 ms later");
    setUpCommandButton(beatForward_, kNudgeBeatLater, "Slip the record one beat later");

    // --- Pitch fader: + toward the DJ (bottom), as on the 1200. 0.05% steps. ---
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
    pitch_.setTooltip("Pitch. While the clip is under the playhead, the music there stays put. "
                      "Double-click for 0 (quartz lock).");
    pitch_.onValueChange = [this] { applyPitch(pitchStepsFromPercent(pitch_.getValue())); };

    addAndMakeVisible(keyShift_);
    keyShift_.setJustificationType(juce::Justification::centred);
    keyShift_.setFont(juce::FontOptions(12.0f));

    setUpCommandButton(pitchUp_, kPitchUp, "Pitch up 0.05%");
    setUpCommandButton(pitchDown_, kPitchDown, "Pitch down 0.05%");
    setUpCommandButton(pitchZero_, kPitchReset, "Pitch to 0 (quartz lock)");

    // --- Channel strip ---
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

void Inspector::setUpCommandButton(juce::TextButton& button, juce::CommandID command, const juce::String& tooltip)
{
    // Buttons run the same commands as the keyboard.
    commandButtons_.push_back({&button, command, tooltip});
    button.setWantsKeyboardFocus(false);
    button.onClick = [this, command] { commands_.invokeDirectly(command, true); };
    addAndMakeVisible(button);
}

void Inspector::refreshShortcuts()
{
    for (const auto& [button, command, tooltip] : commandButtons_)
    {
        const auto keys = commands_.getKeyMappings()->getKeyPressesAssignedToCommand(command);
        button->setTooltip(tooltip + (keys.isEmpty() ? juce::String() : " (" + keys[0].getTextDescription() + ")"));
    }
}

void Inspector::setClip(Id clipId)
{
    clip_ = clipId;
    refresh();
}

juce::String Inspector::harmonyText(const Clip& clip, const Track& track, juce::Colour& colour) const
{
    // The clip this one is mixed with: playing when it comes in, or failing that when it goes out.
    const Set& set = document_.set();
    const ClipTiming timing(clip, track);
    const Clip* other = nullptr;
    for (const auto& candidate : set.clips)
    {
        const Track* t = set.findTrack(candidate.trackId);
        if (candidate.deck == clip.deck || t == nullptr)
            continue;
        try
        {
            const ClipTiming ct(candidate, *t);
            const bool overlaps = ct.startSeconds() < timing.endSeconds() && timing.startSeconds() < ct.endSeconds();
            const bool playingAtStart = ct.startSeconds() <= timing.startSeconds() && ct.endSeconds() > timing.startSeconds();
            if (overlaps && (other == nullptr || playingAtStart))
                other = &candidate;
        }
        catch (const std::invalid_argument&)
        {
        }
    }
    if (other == nullptr)
        return "Not mixed with another track";

    const Track& otherTrack = *set.findTrack(other->trackId);
    const auto mine = pool_.keyFor(track), theirs = pool_.keyFor(otherTrack);
    const juce::String name = "Deck " + juce::String(other->deck + 1) + ": " + juce::String(juce::CharPointer_UTF8(otherTrack.title.c_str()));
    if (!mine || !theirs)
        return "With " + name + ": key unknown";

    const auto a = pitchedKey(*mine, speedFromPitchSteps(clip.pitchSteps));
    const auto b = pitchedKey(*theirs, speedFromPitchSteps(other->pitchSteps));
    const auto relation = harmony(a.key, b.key);
    const double detune = std::abs(a.cents - b.cents);

    juce::String verdict = relation == Harmony::Same ? "same key" : relation == Harmony::Compatible ? "compatible" : "clash";
    colour = relation == Harmony::Clash ? juce::Colours::red
             : detune > 25.0          ? juce::Colours::orange
                                      : juce::Colours::lightgreen;
    if (relation != Harmony::Clash && detune > 25.0)
        verdict << ", but " << juce::String(juce::roundToInt(detune)) << "c out of tune";
    return "With " + name + "\n" + keyText(a.key, display_.keyNotation) + " vs " +
           keyText(b.key, display_.keyNotation) + ": " + verdict;
}

void Inspector::refresh()
{
    const Set& set = document_.set();
    const Clip* clip = set.findClip(clip_);
    const Track* track = clip != nullptr ? set.findTrack(clip->trackId) : nullptr;
    const bool enabled = track != nullptr;

    for (juce::Component* c : getChildren())
        if (c != &title_ && c != &details_)
            c->setEnabled(enabled);

    if (!enabled)
    {
        title_.setText("No clip selected", juce::dontSendNotification);
        details_.setText("Drop audio files on a deck lane, or double-click a file in the library. "
                         "Keys 1-4 select the record on that deck at the playhead.",
                         juce::dontSendNotification);
        for (auto* label : {&tempo_, &key_, &harmony_, &keyShift_, &bpm_})
            label->setText({}, juce::dontSendNotification);
        keyBox_.clear(juce::dontSendNotification);
        return;
    }

    const double speed = speedFromPitchSteps(clip->pitchSteps);
    title_.setText(juce::CharPointer_UTF8(track->title.c_str()), juce::dontSendNotification);
    juce::String details = "Deck " + juce::String(clip->deck + 1) + "   pressed at " + pressedText(track->pressed);
    try
    {
        const ClipTiming timing(*clip, *track);
        details << "\n" << formatTime(timing.startSeconds(), true) << " - " << formatTime(timing.endSeconds(), true)
                << "  (record " << formatTime(static_cast<double>(clip->inFrame) / track->sampleRate, false) << " - "
                << formatTime(static_cast<double>(clip->outFrame) / track->sampleRate, false) << ")";
    }
    catch (const std::invalid_argument& e)
    {
        details << "\n" << e.what();
    }
    details_.setText(details, juce::dontSendNotification);

    // Tempo.
    const auto* entry = pool_.find(track->path);
    const bool analysing = entry == nullptr || entry->status == AudioPool::Status::Loading;
    if (const auto grid = pool_.gridFor(*track))
    {
        bpm_.setText(juce::String(grid->bpm, 2), juce::dontSendNotification);
        tempo_.setText("plays at " + juce::String(grid->bpm * speed, 2) + " BPM" + (track->grid ? "  (corrected)" : ""),
                       juce::dontSendNotification);
    }
    else
    {
        bpm_.setText("?", juce::dontSendNotification);
        tempo_.setText(analysing ? "analysing..." : "no steady beat found; type a BPM", juce::dontSendNotification);
    }

    // Key.
    const auto detected = entry != nullptr ? entry->analysis.key : std::nullopt;
    keyBox_.clear(juce::dontSendNotification);
    keyBox_.addItem("Detected" + (detected ? ": " + keyText(*detected, display_.keyNotation) : juce::String()),
                    kDetectedKeyId);
    for (int minor = 1; minor >= 0; --minor)
        for (int tonic = 0; tonic < 12; ++tonic)
            keyBox_.addItem(keyText({tonic, minor == 1}, display_.keyNotation), 2 + tonic * 2 + minor);
    keyBox_.setSelectedId(track->key ? 2 + track->key->tonic * 2 + (track->key->minor ? 1 : 0) : kDetectedKeyId,
                          juce::dontSendNotification);
    if (const auto key = pool_.keyFor(*track))
        key_.setText("heard as " + pitchedKeyText(*key, speed, display_.keyNotation), juce::dontSendNotification);
    else
        key_.setText(analysing ? "analysing..." : "unknown", juce::dontSendNotification);

    auto colour = juce::Colours::lightgrey;
    harmony_.setText(harmonyText(*clip, *track, colour), juce::dontSendNotification);
    harmony_.setColour(juce::Label::textColourId, colour);

    // Faders.
    pitch_.setValue(pitchPercentFromSteps(clip->pitchSteps), juce::dontSendNotification);
    keyShift_.setText(juce::String::formatted("%+.2f st", semitoneShiftForSpeed(speed) + 0.0), juce::dontSendNotification);
    fader_.setValue(clip->mixer.fader, juce::dontSendNotification);
    high_.setValue(clip->mixer.high, juce::dontSendNotification);
    mid_.setValue(clip->mixer.mid, juce::dontSendNotification);
    low_.setValue(clip->mixer.low, juce::dontSendNotification);
}

void Inspector::applyPitch(int steps)
{
    const Id id = clip_;
    const double pivot = playhead_();
    document_.edit([&](Set& s) { return edit::setClipPitch(s, id, steps, pivot); });
}

void Inspector::applyMixer()
{
    const Id id = clip_;
    const ChannelSettings settings{fader_.getValue(), high_.getValue(), mid_.getValue(), low_.getValue()};
    document_.edit([&](Set& s) { return edit::setClipMixer(s, id, settings); });
}

void Inspector::applyBpm(const juce::String& text)
{
    const Clip* clip = document_.set().findClip(clip_);
    const Track* track = clip != nullptr ? document_.set().findTrack(clip->trackId) : nullptr;
    const double bpm = text.retainCharacters(".0123456789").getDoubleValue();
    if (track == nullptr || bpm <= 0.0)
    {
        refresh();
        return;
    }
    const auto current = pool_.gridFor(*track);
    const BeatGrid grid{bpm, current ? current->firstBeatSeconds : 0.0};
    const Id trackId = track->id;
    if (!document_.edit([&](Set& s) { return edit::setTrackGrid(s, trackId, grid); }))
        refresh();
}

void Inspector::applyKey()
{
    const Clip* clip = document_.set().findClip(clip_);
    if (clip == nullptr)
        return;
    const int id = keyBox_.getSelectedId();
    const auto key = id <= kDetectedKeyId ? std::nullopt
                                          : std::optional(MusicalKey{(id - 2) / 2, (id - 2) % 2 == 1});
    const Id trackId = clip->trackId;
    document_.edit([&](Set& s) { return edit::setTrackKey(s, trackId, key); });
}

void Inspector::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff23262b));
}

void Inspector::resized()
{
    auto area = getLocalBounds().reduced(10);
    auto row = [&](int height) {
        auto r = area.removeFromTop(height);
        area.removeFromTop(3);
        return r;
    };
    auto buttonRow = [](juce::Rectangle<int> r, std::initializer_list<juce::Component*> items) {
        const int width = r.getWidth() / static_cast<int>(items.size());
        for (auto* item : items)
            item->setBounds(r.removeFromLeft(width).reduced(1, 0));
    };

    title_.setBounds(row(22));
    details_.setBounds(row(32));

    tempoCaption_.setBounds(row(16));
    auto tempoRow = row(22);
    bpm_.setBounds(tempoRow.removeFromLeft(64));
    tempo_.setBounds(tempoRow.withTrimmedLeft(6));
    buttonRow(row(22), {&halve_, &double_, &gridEarlier_, &gridLater_});
    buttonRow(row(22), {&downbeat_, &gridReset_});

    keyCaption_.setBounds(row(16));
    auto keyRow = row(22);
    keyBox_.setBounds(keyRow.removeFromLeft(110));
    key_.setBounds(keyRow.withTrimmedLeft(6));
    harmony_.setBounds(row(30));

    nudgeCaption_.setBounds(row(16));
    buttonRow(row(22), {&beatBack_, &back10_, &back1_, &forward1_, &forward10_, &beatForward_});
    area.removeFromTop(6);

    // Pitch column on the left, channel strip on the right.
    auto pitchColumn = area.removeFromLeft(area.getWidth() / 2);
    pitchLabel_.setBounds(pitchColumn.removeFromTop(18));
    auto buttons = pitchColumn.removeFromBottom(24);
    buttonRow(buttons, {&pitchDown_, &pitchZero_, &pitchUp_});
    keyShift_.setBounds(pitchColumn.removeFromBottom(20));
    pitch_.setBounds(pitchColumn);

    auto strip = area;
    const int knobHeight = std::min(76, strip.getHeight() / 5);
    for (auto [knob, label] : {std::pair{&high_, &highLabel_}, std::pair{&mid_, &midLabel_}, std::pair{&low_, &lowLabel_}})
    {
        auto r = strip.removeFromTop(knobHeight);
        label->setBounds(r.removeFromTop(16));
        knob->setBounds(r);
    }
    faderLabel_.setBounds(strip.removeFromTop(18));
    fader_.setBounds(strip);
}

} // namespace vmp

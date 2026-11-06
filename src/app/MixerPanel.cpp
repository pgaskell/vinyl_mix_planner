#include "app/MixerPanel.h"

#include "app/DeckColours.h"
#include "app/Faders.h"

#include "core/Edits.h"
#include "core/Pitch.h"
#include "engine/MixerChannel.h"

namespace vmp
{

namespace
{
constexpr int kStripWidth = 92;
constexpr int kMeterWidth = 11; // two 5 px bars

const juce::Colour kHardwareColour{0xff4fc3f7};
const juce::Colour kWarningColour{0xffff6b5b};
const juce::Colour kRecordingColour{0xffff3b3b};
const juce::Colour kPractiseColour{0xffe8e8e8};
const juce::Colour kTrimColour{0xffffe14d};

juce::String gainText(float gain)
{
    if (gain <= 0.0f)
        return "kill";
    return juce::String::formatted("%+.1f", 20.0 * std::log10(gain) + 0.0);
}

juce::String colorText(double knob)
{
    const auto filter = MixerChannel::colorFilter(knob);
    if (!filter)
        return "off";
    const juce::String hz = filter->hz >= 1000.0 ? juce::String(filter->hz / 1000.0, 1) + " kHz"
                                                 : juce::String(juce::roundToInt(filter->hz)) + " Hz";
    return (filter->type == FilterType::LowPass ? "LPF " : "HPF ") + hz;
}
} // namespace

// --- ChannelStrip -----------------------------------------------------------

ChannelStrip::ChannelStrip(int deck, SetDocument& document, std::function<double()> playhead,
                           const RecordHooks& record)
    : deck_(deck), document_(document), record_(record), playhead_(std::move(playhead)), colour_(deckColour(deck))
{
    deckLabel_.setText("Deck " + juce::String(deck + 1), juce::dontSendNotification);
    deckLabel_.setFont(juce::FontOptions(14.0f, juce::Font::bold));
    deckLabel_.setColour(juce::Label::textColourId, colour_);
    deckLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(deckLabel_);
    addAndMakeVisible(meter_);
    routeLabel_.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    routeLabel_.setJustificationType(juce::Justification::centred);
    routeLabel_.setMinimumHorizontalScale(0.6f);
    addChildComponent(routeLabel_);
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
        knob->textFromValueFunction = [this, param](double v) {
            return trimGrab_[static_cast<size_t>(param)] ? trimText(param, v) : gainText(MixerChannel::eqGain(v)) + " dB";
        };
    }
    setUpControl(color_, colorLabel_, Param::Color);
    color_.textFromValueFunction = [this](double v) {
        return trimGrab_[static_cast<size_t>(Param::Color)] ? trimText(Param::Color, v) : colorText(v);
    };
    color_.setTooltip("Color: a filter for now. Left is a low-pass, right a high-pass; off at the centre.");

    // Not automated: one level for the whole clip.
    trim_.setWantsKeyboardFocus(false);
    trim_.onDragStart = [this] { document_.beginGesture(); };
    trim_.onDragEnd = [this] { document_.endGesture(); };
    trim_.onValueChange = [this] {
        const Id id = clip_;
        document_.edit([&](Set& s) { return edit::setClipTrim(s, id, trim_.getValue()); });
    };
    trim_.textFromValueFunction = [](double v) { return gainText(MixerChannel::trimGain(v)) + " dB"; };
    trim_.setTooltip("Trim: the record's input level, -20 to -4 dB (-12 dB at the centre leaves headroom with the "
                     "faders fully up). One setting for the whole clip.");
    addAndMakeVisible(trim_);
    trimLabel_.setText("Trim", juce::dontSendNotification);
    trimLabel_.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    trimLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(trimLabel_);

    for (auto* knob : {&trim_, &high_, &mid_, &low_, &color_})
    {
        knob->setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        knob->setRange(0.0, 1.0);
        knob->setDoubleClickReturnValue(true, 0.5);
        knob->setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        knob->setPopupDisplayEnabled(true, true, nullptr);
    }

    setUpControl(volume_, volumeLabel_, Param::Volume);
    volume_.setLookAndFeel(&FaderLookAndFeel::instance());
    volume_.setColour(juce::Slider::trackColourId, colour_);
    volume_.setSliderStyle(juce::Slider::LinearVertical);
    volume_.setRange(0.0, 1.0);
    volume_.setDoubleClickReturnValue(true, 1.0);
    volume_.setTextBoxStyle(juce::Slider::TextBoxBelow, true, 40, 16);
    volume_.textFromValueFunction = [this](double v) {
        return trimGrab_[static_cast<size_t>(Param::Volume)] ? trimText(Param::Volume, v)
                                                              : gainText(MixerChannel::faderGain(v));
    };

    show(0, 0.0, {}, {});
}

void ChannelStrip::setUpControl(juce::Slider& slider, juce::Label& label, Param param)
{
    slider.setWantsKeyboardFocus(false);
    // A whole drag undoes as one step.
    slider.onDragStart = [this, &slider, param] {
        document_.beginGesture();
        if (param == Param::Pitch)
            return;
        if (record_.mode && isTrim(record_.mode(deck_, param)))
            trimGrab_[static_cast<size_t>(param)] =
                TrimGrab{slider.getValue(), record_.latchedTrim(deck_, param).value_or(0.0)};
        record(param, slider.getValue()); // taking hold of it starts the pass where it is
    };
    slider.onDragEnd = [this, param] {
        document_.endGesture();
        trimGrab_[static_cast<size_t>(param)].reset();
    };
    slider.onValueChange = [this, &slider, param] {
        apply(param, param == Param::Pitch ? slider.getValue() / kPitchStepPercent : slider.getValue());
    };
    addAndMakeVisible(slider);

    label.setText(paramName(param), juce::dontSendNotification);
    label.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    label.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(label);
}

bool ChannelStrip::record(Param param, double value)
{
    if (!record_.move)
        return false;
    if (record_.mode && isTrim(record_.mode(deck_, param)))
    {
        // How far it has turned since it was taken hold of (or, scrolled, since it was last shown).
        const auto& grab = trimGrab_[static_cast<size_t>(param)];
        const auto from = grab ? *grab : TrimGrab{shown_[static_cast<size_t>(param)],
                                                  record_.latchedTrim(deck_, param).value_or(0.0)};
        return record_.move(deck_, clip_, param, from.amount + value - from.position);
    }
    return record_.move(deck_, clip_, param, value);
}

juce::String ChannelStrip::trimText(Param param, double position) const
{
    const auto& grab = trimGrab_[static_cast<size_t>(param)];
    const double amount = grab ? grab->amount + position - grab->position : 0.0;
    if (param == Param::Color)
        return juce::String::formatted("trim x%.2f", std::exp2(MixerChannel::kTrimColorOctaves * amount));
    return juce::String::formatted("trim %+.1f dB", MixerChannel::kTrimDbPerTurn * amount + 0.0);
}

void ChannelStrip::apply(Param param, double value)
{
    if (param != Param::Pitch && record(param, value))
        return;
    const Id id = clip_;
    const double playhead = playhead_();
    if (param == Param::Pitch) // the records mixed in after this one keep up
        document_.edit([&](Set& s) {
            return edit::withFollowers(s, id, [&](Set& t) { return edit::setParamAt(t, id, param, value, playhead); });
        });
    else
        document_.edit([&](Set& s) { return edit::setParamAt(s, id, param, value, playhead); });
}

ChannelStrip::~ChannelStrip()
{
    volume_.setLookAndFeel(nullptr);
}

void ChannelStrip::show(Id clipId, double playhead, const juce::String& title, const juce::String& key)
{
    clip_ = clipId;
    const Set& set = document_.set();
    const Clip* clip = set.findClip(clipId);
    const Track* track = clip != nullptr ? set.findTrack(clip->trackId) : nullptr;
    for (auto* c : std::initializer_list<juce::Component*>{&volume_, &trim_, &high_, &mid_, &low_, &color_})
        c->setEnabled(track != nullptr);
    title_.setText(track != nullptr ? title : "-", juce::dontSendNotification);
    key_.setText(key, juce::dontSendNotification);
    if (track == nullptr)
    {
        // Nothing on this deck here: neutral and greyed out, not the last clip's values.
        for (auto* slider : {&volume_, &trim_, &high_, &mid_, &low_, &color_})
        {
            slider->setColour(juce::Slider::thumbColourId, getLookAndFeel().findColour(juce::Slider::thumbColourId));
            slider->setColour(juce::Slider::rotarySliderFillColourId,
                              getLookAndFeel().findColour(juce::Slider::rotarySliderFillColourId));
        }
        volume_.setValue(1.0, juce::dontSendNotification);
        for (auto* knob : {&trim_, &high_, &mid_, &low_, &color_})
            knob->setValue(0.5, juce::dontSendNotification);
        return;
    }

    // Values at the playhead, or at the nearer end of the clip when the playhead is outside it.
    const ClipTiming& timing = *document_.timing(clipId);
    const double t = std::clamp(playhead, timing.startSeconds(), timing.endSeconds());
    const double frame = timing.sourceFrameAt(t);

    auto showValue = [&](juce::Slider& slider, Param param, double value) {
        auto latched = param != Param::Pitch && record_.latched ? record_.latched(deck_, param) : std::nullopt;
        const auto trim = param != Param::Pitch && record_.latchedTrim ? record_.latchedTrim(deck_, param) : std::nullopt;
        if (trim)
            latched = MixerChannel::trimmed(param, value, *trim);
        if (latched)
            value = *latched; // the set doesn't have it yet
        if (param != Param::Pitch)
            shown_[static_cast<size_t>(param)] = value;
        const bool automated = !clip->points(param).empty();
        // Coloured by the mode the control was taken hold of in, which it keeps until the pass ends.
        const auto live = trim ? kTrimColour : record_.writing(deck_, param) ? kRecordingColour : kPractiseColour;
        const auto thumb = latched      ? live
                           : automated ? kAutomatedColour
                                       : getLookAndFeel().findColour(juce::Slider::thumbColourId);
        const auto fill = latched      ? live
                          : automated ? kAutomatedColour
                                      : getLookAndFeel().findColour(juce::Slider::rotarySliderFillColourId);
        slider.setColour(juce::Slider::thumbColourId, thumb);
        slider.setColour(juce::Slider::rotarySliderFillColourId, fill);
        if (!slider.isMouseButtonDown()) // don't fight the user's drag
            slider.setValue(value, juce::dontSendNotification);
    };
    if (!trim_.isMouseButtonDown())
        trim_.setValue(clip->mixer.trim, juce::dontSendNotification);
    showValue(volume_, Param::Volume, valueAt(*clip, Param::Volume, frame));
    showValue(high_, Param::High, valueAt(*clip, Param::High, frame));
    showValue(mid_, Param::Mid, valueAt(*clip, Param::Mid, frame));
    showValue(low_, Param::Low, valueAt(*clip, Param::Low, frame));
    showValue(color_, Param::Color, valueAt(*clip, Param::Color, frame));
}

void ChannelStrip::showHardware(const std::optional<HardwareView>& view)
{
    if (view == hardware_)
        return;
    const bool layoutChanged = view.has_value() != hardware_.has_value();
    hardware_ = view;
    if (view)
    {
        routeLabel_.setText(view->route, juce::dontSendNotification);
        routeLabel_.setTooltip(view->tooltip);
        routeLabel_.setColour(juce::Label::textColourId, view->warning ? kWarningColour : kHardwareColour);
    }
    routeLabel_.setVisible(view.has_value());
    if (layoutChanged)
        resized();
    repaint();
}

void ChannelStrip::paintOverChildren(juce::Graphics& g)
{
    if (!hardware_)
        return;
    g.setColour(kHardwareColour);

    // A notch either side of the fader track.
    if (hardware_->fader)
    {
        const auto bounds = volume_.getBounds();
        const float y = static_cast<float>(bounds.getY()) +
                        static_cast<float>(volume_.getPositionOfValue(*hardware_->fader));
        const float centre = static_cast<float>(bounds.getCentreX());
        g.fillRect(juce::Rectangle<float>(centre - 14.0f, y - 1.0f, 7.0f, 2.0f));
        g.fillRect(juce::Rectangle<float>(centre + 7.0f, y - 1.0f, 7.0f, 2.0f));
    }

    // A tick across the knob's arc, placed as LookAndFeel_V4 draws it.
    for (auto [knob, value] : {std::pair{&trim_, hardware_->trim}, std::pair{&high_, hardware_->high},
                               std::pair{&mid_, hardware_->mid}, std::pair{&low_, hardware_->low},
                               std::pair{&color_, hardware_->color}})
    {
        if (!value)
            continue;
        const auto bounds = knob->getBounds().toFloat().reduced(10.0f);
        const float radius = std::min(bounds.getWidth(), bounds.getHeight()) / 2.0f;
        const float lineWidth = std::min(8.0f, radius * 0.5f);
        const auto rotary = knob->getRotaryParameters();
        const float angle = rotary.startAngleRadians +
                            static_cast<float>(*value) * (rotary.endAngleRadians - rotary.startAngleRadians);
        const auto centre = bounds.getCentre();
        const auto inner = centre.getPointOnCircumference(radius - lineWidth - 2.0f, angle);
        const auto outer = centre.getPointOnCircumference(radius + 3.0f, angle);
        g.drawLine({inner, outer}, 2.0f);
    }
}

void ChannelStrip::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff23262b));
    g.setColour(juce::Colour(0xff3a3e45));
    g.drawRect(getLocalBounds());
    g.setColour(colour_);
    g.fillRect(getLocalBounds().removeFromTop(3));
}

void ChannelStrip::resized()
{
    auto area = getLocalBounds().reduced(4);
    deckLabel_.setBounds(area.removeFromTop(18));
    if (hardware_)
        routeLabel_.setBounds(area.removeFromTop(14));
    title_.setBounds(area.removeFromTop(16));
    key_.setBounds(area.removeFromTop(16));
    area.removeFromTop(4);

    const int knobHeight = std::min(60, area.getHeight() / 9);
    for (auto [knob, label] : {std::pair{&trim_, &trimLabel_}, std::pair{&high_, &highLabel_},
                               std::pair{&mid_, &midLabel_}, std::pair{&low_, &lowLabel_},
                               std::pair{&color_, &colorLabel_}})
    {
        label->setBounds(area.removeFromTop(14));
        knob->setBounds(area.removeFromTop(knobHeight - 14));
    }
    area.removeFromTop(6);

    volumeLabel_.setBounds(area.removeFromTop(14));
    meter_.setBounds(area.removeFromRight(kMeterWidth).withTrimmedBottom(18)); // level with the fader track
    volume_.setBounds(area);
}

// --- MixerPanel -------------------------------------------------------------

MixerPanel::MixerPanel(SetDocument& document, AudioPool& pool, const DisplayOptions& display,
                       std::function<double()> playhead)
    : document_(document), pool_(pool), display_(display)
{
    for (int d = 0; d < kMaxDecks; ++d)
    {
        strips_[static_cast<size_t>(d)] = std::make_unique<ChannelStrip>(d, document, playhead, record_);
        addChildComponent(*strips_[static_cast<size_t>(d)]);
    }
    mixMeter_.setTooltip("Level of the internal mix. The light at the top shows it reached full scale (it would "
                         "distort): click it to clear.");
    addAndMakeVisible(mixMeter_);
    mixLabel_.setText("MIX", juce::dontSendNotification);
    mixLabel_.setFont(juce::FontOptions(10.0f, juce::Font::bold));
    mixLabel_.setJustificationType(juce::Justification::centred);
    mixLabel_.setBorderSize({});
    addAndMakeVisible(mixLabel_);
}

void MixerPanel::pushLevels(const std::array<StereoPeak, kMaxDecks>& decks, StereoPeak mix, bool mixActive,
                            double elapsedSeconds)
{
    for (size_t d = 0; d < kMaxDecks; ++d)
        strips_[d]->meter().push(decks[d], elapsedSeconds);
    mixMeter_.setActive(mixActive);
    mixMeter_.push(mix, elapsedSeconds);
}

void MixerPanel::showHardware(const HardwareMixer* mixer, const std::array<int, kMaxDecks>& deckPairs)
{
    for (size_t d = 0; d < kMaxDecks; ++d)
    {
        if (mixer == nullptr)
        {
            strips_[d]->showHardware(std::nullopt);
            continue;
        }

        ChannelStrip::HardwareView view;
        const juce::String mixerName(mixer->profile().name);
        const int pair = deckPairs[d];
        const int channel = pair >= 0 ? mixer->channelForUsbPair(pair) : -1;
        if (pair < 0)
        {
            view.route = "not routed";
            view.warning = true;
            view.tooltip = "This deck isn't sent to an output. Choose one in the Audio menu.";
        }
        else if (channel < 0)
        {
            view.route = "no channel";
            view.warning = true;
            view.tooltip = "Output " + juce::String(2 * pair + 1) + "/" + juce::String(2 * pair + 2) +
                           " doesn't reach a channel on the " + mixerName + ".";
        }
        else
        {
            const auto& state = mixer->channel(channel);
            const juce::String name = "CH" + juce::String(channel + 1);
            view.fader = state.fader;
            view.trim = state.trim;
            view.high = state.high;
            view.mid = state.mid;
            view.low = state.low;
            view.color = state.color;
            const auto& profile = mixer->profile();
            if (isUsbInput(state.input))
            {
                view.route = "> " + name + " " + mixerInputName(state.input, profile.twoUsbPorts);
                view.tooltip = "Plays on " + name + " of the " + mixerName;
                if (profile.twoUsbPorts)
                    view.tooltip << " if this Mac is on its " << mixerInputName(state.input, true) << " port";
                view.tooltip << ". The blue marks show where its controls are.";
            }
            else if (state.input == MixerInput::Unknown)
            {
                view.route = "> " + name;
                view.tooltip = "Plays on " + name + " of the " + mixerName + " if its input selector is on USB.";
            }
            else
            {
                view.route = name + " on " + mixerInputName(state.input, profile.twoUsbPorts);
                view.warning = true;
                view.tooltip = "Plays on " + name + " of the " + mixerName +
                               ", but its input selector isn't on USB: you won't hear it.";
            }
            if (!state.fader && !state.high)
                view.tooltip << (profile.snapshotKnown
                                     ? " To see its controls here, turn on the mixer's MIDI (ON/OFF button) and "
                                       "hold START/STOP for a second."
                                     : " To see its controls here, turn on the mixer's MIDI (ON/OFF button) and "
                                       "move them, or try holding START/STOP for a second.");
        }
        strips_[d]->showHardware(view);
    }
}

void MixerPanel::showHardwareControl(const HardwareMixer& mixer)
{
    const juce::String mixerName(mixer.profile().name);
    for (size_t d = 0; d < kMaxDecks; ++d)
    {
        if (d >= mixer.profile().channels.size())
        {
            strips_[d]->showHardware(std::nullopt);
            continue;
        }
        const auto& state = mixer.channel(static_cast<int>(d));
        const juce::String name = "CH" + juce::String(static_cast<int>(d) + 1);
        ChannelStrip::HardwareView view;
        view.fader = state.fader;
        view.trim = state.trim;
        view.high = state.high;
        view.mid = state.mid;
        view.low = state.low;
        view.color = state.color;
        view.route = "< " + name;
        view.tooltip = "The " + mixerName + "'s " + name +
                       " fader, trim, EQ and COLOR play this deck, shown by the blue marks. The set keeps its own "
                       "values; turn this off in the Audio menu.";
        if (!state.fader && !state.high)
            view.tooltip << " Nothing has arrived yet: turn on the mixer's MIDI (ON/OFF button) and hold "
                            "START/STOP for a second to send every position.";
        strips_[d]->showHardware(view);
    }
}

int MixerPanel::preferredWidth(int numDecks)
{
    return kStripWidth * numDecks + kMixMeterWidth;
}

void MixerPanel::update(Id selectedClip, double playhead, bool playing)
{
    const Set& set = document_.set();
    if (set.numDecks != shownDecks_)
    {
        shownDecks_ = set.numDecks;
        resized();
    }

    for (int d = 0; d < set.numDecks; ++d)
    {
        const Clip* chosen = set.findClip(clipForDeck(document_, d, selectedClip, playhead, playing));

        juce::String title, key;
        if (chosen != nullptr)
        {
            const Track& track = *set.findTrack(chosen->trackId);
            title = juce::CharPointer_UTF8(track.title.c_str());
            if (const auto k = pool_.keyFor(track))
            {
                const ClipTiming& timing = *document_.timing(chosen->id);
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
    auto mix = area.removeFromRight(kMixMeterWidth).reduced(4, 4);
    mixLabel_.setBounds(mix.removeFromTop(16));
    mixMeter_.setBounds(mix.withTrimmedBottom(18));
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

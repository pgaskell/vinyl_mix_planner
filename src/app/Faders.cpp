#include "app/Faders.h"

#include "app/DeckColours.h"
#include "core/Edits.h"
#include "core/Pitch.h"

namespace vmp
{

Id clipForDeck(const Set& set, int deck, Id selectedClip, double playhead, bool playing)
{
    const Clip* selected = set.findClip(selectedClip);
    Id chosen = selected != nullptr && selected->deck == deck ? selected->id : 0;
    if (chosen == 0 || playing)
        for (const auto& clip : set.clips)
            if (const Track* track = set.findTrack(clip.trackId); clip.deck == deck && track != nullptr)
            {
                const ClipTiming timing(clip, *track);
                if (playhead >= timing.startSeconds() && playhead < timing.endSeconds())
                    chosen = clip.id;
            }
    return chosen;
}

// --- FaderLookAndFeel -------------------------------------------------------

namespace
{
constexpr float kCapHeight = 14.0f;
}

FaderLookAndFeel& FaderLookAndFeel::instance()
{
    static FaderLookAndFeel lookAndFeel;
    return lookAndFeel;
}

int FaderLookAndFeel::getSliderThumbRadius(juce::Slider& slider)
{
    return slider.isVertical() ? static_cast<int>(kCapHeight / 2.0f) + 1 : LookAndFeel_V4::getSliderThumbRadius(slider);
}

void FaderLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                        float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle style,
                                        juce::Slider& slider)
{
    if (style != juce::Slider::LinearVertical)
    {
        LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }
    const auto area = juce::Rectangle<int>(x, y, width, height).toFloat();
    const float centre = area.getCentreX();

    // The groove, in the track colour.
    g.setColour(slider.findColour(juce::Slider::trackColourId).withMultipliedAlpha(slider.isEnabled() ? 0.8f : 0.3f));
    g.fillRoundedRectangle(centre - 2.0f, area.getY(), 4.0f, area.getHeight(), 2.0f);

    // The cap: wide and flat, with a line across where it reads.
    const float capWidth = std::min(area.getWidth() - 2.0f, 40.0f);
    const auto cap = juce::Rectangle<float>(capWidth, kCapHeight).withCentre({centre, sliderPos});
    const auto colour = slider.findColour(juce::Slider::thumbColourId).withMultipliedAlpha(slider.isEnabled() ? 1.0f : 0.4f);
    g.setColour(juce::Colours::black.withAlpha(0.5f));
    g.fillRoundedRectangle(cap.translated(0.0f, 1.5f), 2.5f); // shadow
    g.setColour(colour);
    g.fillRoundedRectangle(cap, 2.5f);
    g.setColour(colour.contrasting(0.6f));
    g.fillRect(cap.getX() + 3.0f, sliderPos - 0.75f, cap.getWidth() - 6.0f, 1.5f);
}

// --- PitchFader -------------------------------------------------------------

PitchFader::PitchFader(int deck, SetDocument& document, std::function<double()> playhead)
    : document_(document), playhead_(std::move(playhead))
{
    slider_.setLookAndFeel(&FaderLookAndFeel::instance());
    slider_.setSliderStyle(juce::Slider::LinearVertical);
    slider_.setNormalisableRange(juce::NormalisableRange<double>(
        -8.0, 8.0, [](double, double, double proportion) { return 8.0 - 16.0 * proportion; },
        [](double, double, double value) { return (8.0 - value) / 16.0; },
        [](double, double, double value) { return std::round(value / kPitchStepPercent) * kPitchStepPercent; }));
    slider_.setDoubleClickReturnValue(true, 0.0);
    slider_.setColour(juce::Slider::trackColourId, deckColour(deck));
    slider_.setTextBoxStyle(juce::Slider::TextBoxBelow, true, 46, 16);
    slider_.textFromValueFunction = [](double v) { return juce::String::formatted("%+.2f", v + 0.0); };
    slider_.valueFromTextFunction = [](const juce::String& s) {
        return s.retainCharacters("+-.0123456789").getDoubleValue();
    };
    slider_.setWantsKeyboardFocus(false);
    slider_.setTooltip("Pitch (+ at the bottom, like a 1200). The records mixed in after this one follow.");
    // A whole drag undoes as one step.
    slider_.onDragStart = [this] { document_.beginGesture(); };
    slider_.onDragEnd = [this] { document_.endGesture(); };
    slider_.onValueChange = [this] {
        const Id id = clip_;
        const double steps = slider_.getValue() / kPitchStepPercent;
        const double at = playhead_();
        document_.edit([&](Set& set) {
            return edit::withFollowers(set, id, [&](Set& t) { return edit::setParamAt(t, id, Param::Pitch, steps, at); });
        });
    };
    addAndMakeVisible(slider_);
    label_.setText("PITCH", juce::dontSendNotification);
    label_.setFont(juce::FontOptions(10.0f, juce::Font::bold));
    label_.setJustificationType(juce::Justification::centred);
    label_.setBorderSize({});
    label_.setColour(juce::Label::textColourId, deckColour(deck));
    label_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(label_);
    show(0, 0.0);
}

PitchFader::~PitchFader()
{
    slider_.setLookAndFeel(nullptr);
}

void PitchFader::show(Id clipId, double playhead)
{
    clip_ = clipId;
    const Set& set = document_.set();
    const Clip* clip = set.findClip(clipId);
    const Track* track = clip != nullptr ? set.findTrack(clip->trackId) : nullptr;
    slider_.setEnabled(track != nullptr);
    const bool automated = track != nullptr && !clip->points(Param::Pitch).empty();
    slider_.setColour(juce::Slider::thumbColourId,
                      automated ? kAutomatedColour : getLookAndFeel().findColour(juce::Slider::thumbColourId));
    if (slider_.isMouseButtonDown()) // don't fight the user's drag
        return;
    if (track == nullptr)
    {
        slider_.setValue(0.0, juce::dontSendNotification);
        return;
    }
    // The pitch at the playhead, or at the nearer end of the clip when the playhead is outside it.
    const ClipTiming timing(*clip, *track);
    const double t = std::clamp(playhead, timing.startSeconds(), timing.endSeconds());
    slider_.setValue((timing.speedAt(t) - 1.0) * 100.0, juce::dontSendNotification);
}

void PitchFader::resized()
{
    auto area = getLocalBounds();
    label_.setBounds(area.removeFromTop(12));
    slider_.setBounds(area);
}

} // namespace vmp

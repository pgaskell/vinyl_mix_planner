#include "app/GlitchPanel.h"

#include "app/TimeFormat.h"

namespace vmp
{

namespace
{
constexpr double kFlashMs = 1200.0;

juce::String describe(const Glitch& glitch)
{
    const juce::String deck = glitch.deck >= 0 ? "Deck " + juce::String(glitch.deck + 1) + ": " : juce::String();
    switch (glitch.kind)
    {
    case GlitchKind::Overload:
        return "Engine overload: took " + juce::String(glitch.usedMs, 1) + " ms to render " +
               juce::String(glitch.budgetMs, 1) + " ms of audio";
    case GlitchKind::DriverDropout:
        return "The audio driver reported a dropout";
    case GlitchKind::NotLoaded:
        return deck + "the clip's audio wasn't loaded, so it played silence";
    case GlitchKind::Underrun:
        return deck + "audio wasn't read from disk in time";
    }
    return {};
}
} // namespace

GlitchPanel::GlitchPanel()
{
    list_.setRowHeight(22);
    list_.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff23262b));
    addAndMakeVisible(list_);

    clearButton_.setWantsKeyboardFocus(false);
    clearButton_.onClick = [this] { clear(); };
    addAndMakeVisible(clearButton_);

    summary_.setFont(juce::FontOptions(12.0f));
    summary_.setColour(juce::Label::textColourId, juce::Colours::grey);
    summary_.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(summary_);
    changed();
}

void GlitchPanel::add(const std::vector<Glitch>& glitches)
{
    if (glitches.empty())
        return;
    const auto now = juce::Time::getCurrentTime();
    for (const auto& glitch : glitches)
        entries_.push_back({glitch, now});
    if (entries_.size() > kMaxEntries)
    {
        const auto excess = entries_.size() - kMaxEntries;
        lost_ += static_cast<uint32_t>(excess);
        entries_.erase(entries_.begin(), entries_.begin() + static_cast<long>(excess));
    }
    changed();
    list_.scrollToEnsureRowIsOnscreen(static_cast<int>(entries_.size()) - 1);
}

void GlitchPanel::addLost(uint32_t count)
{
    if (count == 0)
        return;
    lost_ += count;
    changed();
}

void GlitchPanel::clear()
{
    entries_.clear();
    lost_ = 0;
    changed();
}

std::vector<double> GlitchPanel::positions() const
{
    std::vector<double> seconds;
    seconds.reserve(entries_.size());
    for (const auto& entry : entries_)
        seconds.push_back(entry.glitch.seconds);
    return seconds;
}

void GlitchPanel::changed()
{
    list_.updateContent();
    list_.repaint();
    juce::String text = juce::String(count()) + (count() == 1 ? " glitch" : " glitches");
    if (lost_ > 0)
        text += " (" + juce::String(lost_) + " not listed)";
    summary_.setText(text, juce::dontSendNotification);
    clearButton_.setEnabled(count() > 0);
    if (onChanged)
        onChanged();
}

void GlitchPanel::resized()
{
    auto area = getLocalBounds();
    auto bar = area.removeFromRight(170).reduced(6, 4);
    clearButton_.setBounds(bar.removeFromTop(24).removeFromRight(60));
    summary_.setBounds(bar.removeFromTop(36));
    list_.setBounds(area);
}

void GlitchPanel::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (selected)
        g.fillAll(juce::Colour(0xff3a3e45));
    g.setFont(juce::FontOptions(13.0f));

    if (entries_.empty())
    {
        g.setColour(juce::Colours::grey);
        g.drawText("No glitches. Dropouts and unexpected silences while playing are listed here.", 8, 0, width - 16,
                   height, juce::Justification::centredLeft);
        return;
    }

    const auto& entry = entries_[static_cast<size_t>(row)];
    g.setColour(juce::Colours::grey);
    g.drawText(entry.when.formatted("%H:%M:%S"), 8, 0, 64, height, juce::Justification::centredLeft);
    g.setColour(juce::Colours::orange);
    g.drawText(formatTime(entry.glitch.seconds, true), 76, 0, 80, height, juce::Justification::centredLeft);
    g.setColour(juce::Colours::white);
    g.drawText(describe(entry.glitch), 160, 0, width - 168, height, juce::Justification::centredLeft, true);
}

void GlitchPanel::listBoxItemClicked(int row, const juce::MouseEvent&)
{
    if (row >= 0 && row < static_cast<int>(entries_.size()) && onGlitchClicked)
        onGlitchClicked(entries_[static_cast<size_t>(row)].glitch.seconds);
}

// --- GlitchLight ------------------------------------------------------------

GlitchLight::GlitchLight() : juce::Button("Glitches")
{
    setWantsKeyboardFocus(false);
}

void GlitchLight::setCount(int count)
{
    if (count > count_)
        flashedAt_ = juce::Time::getMillisecondCounterHiRes();
    count_ = count;
    update();
    repaint();
}

void GlitchLight::update()
{
    const double age = juce::Time::getMillisecondCounterHiRes() - flashedAt_;
    const float flash = age < kFlashMs ? 1.0f - static_cast<float>(age / kFlashMs) : 0.0f;
    if (std::abs(flash - flash_) > 0.0f)
    {
        flash_ = flash;
        repaint();
    }
}

void GlitchLight::paintButton(juce::Graphics& g, bool highlighted, bool)
{
    auto area = getLocalBounds().toFloat();
    if (highlighted)
    {
        g.setColour(juce::Colours::white.withAlpha(0.08f));
        g.fillRoundedRectangle(area, 4.0f);
    }

    const auto led = area.removeFromLeft(area.getHeight()).reduced(area.getHeight() * 0.25f);
    const auto off = juce::Colour(0xff4a4e55);
    const auto dim = juce::Colour(0xff8a2a2a);
    const auto lit = juce::Colour(0xffff3b30);
    g.setColour(count_ == 0 ? off : dim.interpolatedWith(lit, flash_));
    g.fillEllipse(led);
    if (flash_ > 0.0f)
    {
        g.setColour(lit.withAlpha(0.35f * flash_));
        g.fillEllipse(led.expanded(4.0f * flash_));
    }

    g.setColour(count_ == 0 ? juce::Colours::grey : juce::Colours::white);
    g.setFont(juce::FontOptions(13.0f));
    g.drawText(juce::String(count_), area.withTrimmedLeft(2.0f), juce::Justification::centredLeft);
}

} // namespace vmp

#include "app/LevelMeter.h"

#include <cmath>

namespace vmp
{

namespace
{
constexpr float kFloorDb = -48.0f, kTopDb = 6.0f;
constexpr float kFallDbPerSecond = 24.0f;
constexpr double kHoldSeconds = 1.5;
constexpr float kClipLightHeight = 6.0f;

float toDb(float level)
{
    return level > 0.0f ? 20.0f * std::log10(level) : -100.0f;
}
} // namespace

LevelMeter::LevelMeter()
{
    setTooltip("Peak level, left and right. A light at the top shows that side reached full scale: click to clear.");
}

bool LevelMeter::Side::push(float peak, double elapsedSeconds)
{
    const float db = toDb(peak);
    const float level = std::max({db, levelDb - kFallDbPerSecond * static_cast<float>(elapsedSeconds), -100.0f});
    holdAge += elapsedSeconds;
    const bool newHold = db >= holdDb || holdAge > kHoldSeconds;
    if (newHold)
    {
        holdDb = db;
        holdAge = 0.0;
    }
    const bool wasClipped = clipped;
    clipped = clipped || peak >= 1.0f;
    const bool changed = std::abs(level - levelDb) > 0.05f || clipped != wasClipped || newHold;
    levelDb = level;
    return changed;
}

void LevelMeter::push(StereoPeak peak, double elapsedSeconds)
{
    const bool left = sides_[0].push(peak.left, elapsedSeconds);
    const bool right = sides_[1].push(peak.right, elapsedSeconds);
    if (left || right)
        repaint();
}

void LevelMeter::setActive(bool active)
{
    if (active == active_)
        return;
    active_ = active;
    if (!active)
        for (auto& side : sides_)
            side.levelDb = side.holdDb = -100.0f;
    repaint();
}

void LevelMeter::paint(juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();
    const float width = (area.getWidth() - 1.0f) / 2.0f;
    paintSide(g, sides_[0], area.removeFromLeft(width));
    paintSide(g, sides_[1], area.withTrimmedLeft(1.0f));
}

void LevelMeter::paintSide(juce::Graphics& g, const Side& side, juce::Rectangle<float> area) const
{
    const auto light = area.removeFromTop(kClipLightHeight);
    area.removeFromTop(2.0f);
    g.setColour(side.clipped ? juce::Colour(0xffff3b30) : juce::Colour(0xff3a2a2a));
    g.fillRect(light);
    g.setColour(juce::Colour(0xff111317));
    g.fillRect(area);
    if (!active_)
        return;

    auto yOf = [&](float db) {
        const float proportion = (std::clamp(db, kFloorDb, kTopDb) - kFloorDb) / (kTopDb - kFloorDb);
        return area.getBottom() - proportion * area.getHeight();
    };

    // Green to -12 dB, yellow to 0 dB, red above.
    const float top = yOf(side.levelDb);
    for (auto [from, to, colour] : {std::tuple{kFloorDb, -12.0f, juce::Colour(0xff35d05a)},
                                    std::tuple{-12.0f, 0.0f, juce::Colour(0xffffd23f)},
                                    std::tuple{0.0f, kTopDb, juce::Colour(0xffff3b30)}})
    {
        const float y0 = std::max(top, yOf(to)), y1 = yOf(from);
        if (y1 > y0)
        {
            g.setColour(colour);
            g.fillRect(area.getX(), y0, area.getWidth(), y1 - y0);
        }
    }
    if (side.holdDb > kFloorDb)
    {
        g.setColour(juce::Colours::white);
        g.fillRect(area.getX(), yOf(side.holdDb) - 1.0f, area.getWidth(), 1.5f);
    }
    g.setColour(juce::Colours::white.withAlpha(0.35f)); // 0 dB mark
    g.fillRect(area.getX(), yOf(0.0f), area.getWidth(), 1.0f);
}

void LevelMeter::mouseDown(const juce::MouseEvent&)
{
    for (auto& side : sides_)
        side.clipped = false;
    repaint();
}

} // namespace vmp

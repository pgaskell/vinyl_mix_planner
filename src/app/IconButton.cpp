#include "app/IconButton.h"

namespace vmp
{

namespace
{
// The symbol in a 16 x 16 box.
juce::Path iconPath(Icon icon)
{
    juce::Path p;
    auto triangleRight = [&](float x, float width) { p.addTriangle(x, 2.0f, x, 14.0f, x + width, 8.0f); };
    auto triangleLeft = [&](float x, float width) { p.addTriangle(x + width, 2.0f, x + width, 14.0f, x, 8.0f); };
    switch (icon)
    {
    case Icon::PreviousMix:
        p.addRectangle(2.0f, 2.0f, 2.0f, 12.0f);
        triangleLeft(4.0f, 10.0f);
        break;
    case Icon::Rewind:
        triangleLeft(1.0f, 7.0f);
        triangleLeft(8.0f, 7.0f);
        break;
    case Icon::Play:
        triangleRight(3.0f, 11.0f);
        break;
    case Icon::Pause:
        p.addRectangle(3.0f, 2.0f, 3.5f, 12.0f);
        p.addRectangle(9.5f, 2.0f, 3.5f, 12.0f);
        break;
    case Icon::Stop:
        p.addRectangle(3.0f, 3.0f, 10.0f, 10.0f);
        break;
    case Icon::FastForward:
        triangleRight(1.0f, 7.0f);
        triangleRight(8.0f, 7.0f);
        break;
    case Icon::NextMix:
        triangleRight(2.0f, 10.0f);
        p.addRectangle(12.0f, 2.0f, 2.0f, 12.0f);
        break;
    case Icon::ZoomIn:
    case Icon::ZoomOut:
    {
        // A magnifying glass with + or -.
        juce::Path outline;
        outline.addEllipse(1.5f, 1.5f, 9.5f, 9.5f);
        outline.startNewSubPath(9.8f, 9.8f);
        outline.lineTo(14.5f, 14.5f);
        outline.startNewSubPath(3.8f, 6.25f);
        outline.lineTo(8.7f, 6.25f);
        if (icon == Icon::ZoomIn)
        {
            outline.startNewSubPath(6.25f, 3.8f);
            outline.lineTo(6.25f, 8.7f);
        }
        juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded)
            .createStrokedPath(p, outline);
        break;
    }
    case Icon::MouseHelp:
    {
        // A pointer, and an "i" in a ring below and to its right.
        p.startNewSubPath(1.0f, 0.5f);
        p.lineTo(1.0f, 10.5f);
        p.lineTo(3.4f, 8.3f);
        p.lineTo(5.0f, 12.0f);
        p.lineTo(6.6f, 11.3f);
        p.lineTo(5.0f, 7.6f);
        p.lineTo(8.4f, 7.6f);
        p.closeSubPath();
        juce::Path circle, ring;
        circle.addEllipse(8.0f, 8.0f, 7.5f, 7.5f);
        juce::PathStrokeType(1.3f).createStrokedPath(ring, circle);
        p.addPath(ring);
        p.addEllipse(11.1f, 9.7f, 1.3f, 1.3f);
        p.addRectangle(11.15f, 11.6f, 1.2f, 2.6f);
        break;
    }
    }
    return p;
}
} // namespace

IconButton::IconButton(Icon icon) : juce::Button({}), icon_(icon) {}

void IconButton::setIcon(Icon icon)
{
    if (icon == icon_)
        return;
    icon_ = icon;
    repaint();
}

void IconButton::paintButton(juce::Graphics& g, bool highlighted, bool down)
{
    auto& lf = getLookAndFeel();
    lf.drawButtonBackground(g, *this, findColour(juce::TextButton::buttonColourId), highlighted, down);

    const float size = std::min(14.0f, static_cast<float>(std::min(getWidth(), getHeight())) - 8.0f);
    const auto box = getLocalBounds().toFloat().withSizeKeepingCentre(size, size);
    auto path = iconPath(icon_);
    path.applyTransform(juce::AffineTransform::scale(size / 16.0f).translated(box.getX(), box.getY()));
    g.setColour(findColour(juce::TextButton::textColourOffId).withMultipliedAlpha(isEnabled() ? 1.0f : 0.4f));
    g.fillPath(path);
}

} // namespace vmp

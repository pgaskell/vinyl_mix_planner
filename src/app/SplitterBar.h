#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace vmp
{

// A thin vertical edge between two panels that can be dragged sideways. Reports how far it has
// been dragged since the mouse went down; the owner works out the new layout.
class SplitterBar : public juce::Component
{
public:
    std::function<void()> onDragStart;
    std::function<void(int deltaX)> onDrag;

    SplitterBar() { setMouseCursor(juce::MouseCursor::LeftRightResizeCursor); }

    void paint(juce::Graphics& g) override
    {
        g.setColour(isMouseOverOrDragging() ? juce::Colour(0xff5a8dee) : juce::Colour(0xff2c3036));
        g.fillRect(getLocalBounds().withSizeKeepingCentre(isMouseOverOrDragging() ? 3 : 1, getHeight()));
    }
    void mouseEnter(const juce::MouseEvent&) override { repaint(); }
    void mouseExit(const juce::MouseEvent&) override { repaint(); }
    void mouseDown(const juce::MouseEvent&) override
    {
        if (onDragStart)
            onDragStart();
    }
    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (onDrag)
            onDrag(e.getDistanceFromDragStartX());
    }
    void mouseUp(const juce::MouseEvent&) override { repaint(); }
};

} // namespace vmp

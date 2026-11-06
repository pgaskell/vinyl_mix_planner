#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace vmp
{

// What clicking and dragging does in the timeline, and what the modifier keys change: shown in a
// callout from the toolbar. Keep it in step with TimelineView's class comment.
class MouseHelp : public juce::Component
{
public:
    MouseHelp();
    void paint(juce::Graphics& g) override;

private:
    juce::TextLayout layout_;
};

} // namespace vmp

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace vmp
{

enum class Icon
{
    PreviousMix,
    Rewind,
    Play,
    Pause,
    Stop,
    FastForward,
    NextMix,
    ZoomIn,
    ZoomOut,
    MouseHelp // a pointer with an "i"
};

// A button showing one of the usual symbols instead of text, drawn as a vector path on the
// normal button background. The tooltip says what it does.
class IconButton : public juce::Button
{
public:
    explicit IconButton(Icon icon);

    void setIcon(Icon icon);
    void paintButton(juce::Graphics& g, bool highlighted, bool down) override;

private:
    Icon icon_;
};

} // namespace vmp

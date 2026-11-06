#include "app/MouseHelp.h"

namespace vmp
{

namespace
{
constexpr float kWidth = 520.0f, kMargin = 14.0f;

// Sections of (gesture, what it does).
struct Section
{
    const char* title;
    std::vector<std::pair<const char*, const char*>> lines;
};

const std::vector<Section>& sections()
{
    static const std::vector<Section> all{
        {"Records",
         {{"Drag", "move it in time or to another deck (Shift: finely)"},
          {"Drag an edge", "trim where the needle drops or lifts"},
          {"Shift-click", "make the nearest beat line the downbeat (Option-Shift: exactly where clicked)"},
          {"Drop files", "from the library or the Finder onto a lane"}}},
        {"While it plays, your hand is on the record",
         {{"Click", "select it"},
          {"Press or drag", "hold it still or push it on, recorded as a touch"},
          {"Option-drag", "the tone arm: move the record (stopped too). Breaks its beat links"}}},
        {"Beat grid: Cmd",
         {{"Cmd-click", "the downbeat is about here"},
          {"Cmd-Shift-click", "another beat is about here (the next one is best): the BPM is found from the music"},
          {"Cmd-drag a beat line", "pin it where the beat really is (Shift: finely). A pin's handle drags without "
                                   "Cmd; double-click it to unpin"},
          {"Cmd-drag into another lane", "link two moments to be heard together. Drag a link's dot to move it; "
                                         "double-click to delete"}}},
        {"Nudges and holds",
         {{"Drag", "move one along the music, when stopped (Shift: finely)"},
          {"Drag its number", "up or down to change how far it moves the music"},
          {"Double-click", "delete it"}}},
        {"Automation lines",
         {{"Click a line", "add a keypoint; drag keypoints to move them"},
          {"Double-click a keypoint", "delete it"}}},
        {"Timeline",
         {{"Click the ruler", "move the playhead"},
          {"Scroll", "pan; Cmd-scroll or pinch to zoom"},
          {"Right-click", "anything for more: inserting nudges, cleaning them up, beat links, keypoints"}}},
    };
    return all;
}
} // namespace

MouseHelp::MouseHelp()
{
    const auto text = juce::Colour(0xffe6e8eb), dim = juce::Colour(0xffb4b9c0), heading = juce::Colour(0xff8fb8ff);
    const juce::Font body(juce::FontOptions(13.5f)), bold = body.boldened(),
        title(juce::FontOptions(14.5f, juce::Font::bold));
    juce::AttributedString help;
    help.setLineSpacing(2.0f);
    bool first = true;
    for (const auto& section : sections())
    {
        help.append(juce::String(first ? "" : "\n") + section.title + "\n", title, heading);
        first = false;
        for (const auto& [gesture, effect] : section.lines)
        {
            help.append(juce::String("  ") + gesture, bold, text);
            help.append(juce::String(": ") + effect + "\n", body, dim);
        }
    }
    layout_.createLayout(help, kWidth - 2 * kMargin);
    setSize(static_cast<int>(kWidth), static_cast<int>(std::ceil(layout_.getHeight() + 2 * kMargin)));
}

void MouseHelp::paint(juce::Graphics& g)
{
    layout_.draw(g, getLocalBounds().toFloat().reduced(kMargin));
}

} // namespace vmp

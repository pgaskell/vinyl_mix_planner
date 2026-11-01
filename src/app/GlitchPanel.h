#pragma once

#include "engine/Glitches.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace vmp
{

// Dropouts and unexpected silences heard while playing, oldest first. Clicking one moves the
// playhead there. Kept for this session only.
class GlitchPanel : public juce::Component, private juce::ListBoxModel
{
public:
    GlitchPanel();

    // Keeps the most recent kMaxEntries; older ones are counted but not listed.
    static constexpr size_t kMaxEntries = 1000;

    void add(const std::vector<Glitch>& glitches);
    // Glitches the engine couldn't record because too many happened at once.
    void addLost(uint32_t count);
    void clear();

    int count() const { return static_cast<int>(entries_.size()) + static_cast<int>(lost_); }
    // Timeline positions, for marking on the ruler.
    std::vector<double> positions() const;

    std::function<void(double seconds)> onGlitchClicked;
    std::function<void()> onChanged;

    void resized() override;

private:
    struct Entry
    {
        Glitch glitch;
        juce::Time when;
    };

    int getNumRows() override { return std::max<int>(1, static_cast<int>(entries_.size())); }
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;
    void changed();

    juce::ListBox list_{"Glitches", this};
    juce::TextButton clearButton_{"Clear"};
    juce::Label summary_;
    std::vector<Entry> entries_;
    uint32_t lost_ = 0;
};

// The toolbar light: grey when nothing has gone wrong, flashes red on each glitch and stays dim
// red while any are listed. Shows the count; click it to see the list.
class GlitchLight : public juce::Button
{
public:
    GlitchLight();

    void setCount(int count);
    // Call from the UI timer: fades the flash.
    void update();

    void paintButton(juce::Graphics& g, bool highlighted, bool down) override;

private:
    int count_ = 0;
    double flashedAt_ = -1.0e9; // ms
    float flash_ = 0.0f;
};

} // namespace vmp

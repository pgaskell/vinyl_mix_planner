#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <deque>
#include <vector>

namespace vmp
{

// Incoming MIDI, newest at the bottom. A control sending a run of values (a fader sweep is
// ~500 messages a second) is one line that updates, with a count. Only visible rows are drawn.
class MidiMonitor : public juce::Component, private juce::ListBoxModel
{
public:
    MidiMonitor();

    static constexpr size_t kMaxEntries = 1000;

    // MIDI thread.
    void add(const juce::String& source, const juce::MidiMessage& message);
    // Message thread, from the UI timer: shows what has arrived.
    void update();

    void resized() override;

private:
    struct Pending
    {
        juce::String source;
        juce::MidiMessage message;
        juce::Time when;
    };
    struct Entry
    {
        juce::String source;
        juce::MidiMessage message;
        juce::Time when;
        int count = 1;
    };

    int getNumRows() override { return static_cast<int>(entries_.size()); }
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;

    static constexpr size_t kMaxPending = 10000; // if the UI stalls, drop rather than grow

    juce::CriticalSection lock_;
    std::vector<Pending> pending_; // guarded by lock_
    std::vector<Pending> incoming_;
    std::deque<Entry> entries_;
    const juce::Font rowFont_{juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain)};
    juce::ListBox list_{"MIDI", this};
};

} // namespace vmp

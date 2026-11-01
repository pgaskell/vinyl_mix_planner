#include "app/MidiMonitor.h"

namespace vmp
{

namespace
{
// Messages that are the same control sending a new value.
bool sameControl(const juce::MidiMessage& a, const juce::MidiMessage& b)
{
    if (a.isController() && b.isController())
        return a.getChannel() == b.getChannel() && a.getControllerNumber() == b.getControllerNumber();
    if (a.isPitchWheel() && b.isPitchWheel())
        return a.getChannel() == b.getChannel();
    return false;
}
} // namespace

MidiMonitor::MidiMonitor()
{
    list_.setRowHeight(16);
    list_.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff1b1d21));
    addAndMakeVisible(list_);
}

void MidiMonitor::add(const juce::String& source, const juce::MidiMessage& message)
{
    const juce::ScopedLock lock(lock_);
    if (pending_.size() < kMaxPending)
        pending_.push_back({source, message, juce::Time::getCurrentTime()});
}

void MidiMonitor::update()
{
    {
        const juce::ScopedLock lock(lock_);
        incoming_.swap(pending_);
    }
    if (incoming_.empty())
        return;

    for (auto& in : incoming_)
    {
        if (!entries_.empty() && entries_.back().source == in.source && sameControl(entries_.back().message, in.message))
        {
            auto& last = entries_.back();
            last.message = in.message;
            last.when = in.when;
            ++last.count;
            continue;
        }
        entries_.push_back({std::move(in.source), in.message, in.when});
    }
    incoming_.clear();
    while (entries_.size() > kMaxEntries)
        entries_.pop_front();

    list_.updateContent();
    list_.repaint();
    list_.scrollToEnsureRowIsOnscreen(static_cast<int>(entries_.size()) - 1);
}

void MidiMonitor::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= static_cast<int>(entries_.size()))
        return;
    if (selected)
        g.fillAll(juce::Colour(0xff3a3e45));
    const auto& e = entries_[static_cast<size_t>(row)];
    juce::String text = e.when.formatted("%H:%M:%S") + "  " + e.source + "  " +
                        juce::String::toHexString(e.message.getRawData(), e.message.getRawDataSize()) + "  " +
                        e.message.getDescription();
    if (e.count > 1)
        text << "  (x" << e.count << ")";
    g.setFont(rowFont_);
    g.setColour(juce::Colours::white.withAlpha(0.85f));
    g.drawText(text, 6, 0, width - 12, height, juce::Justification::centredLeft, true);
}

void MidiMonitor::resized()
{
    list_.setBounds(getLocalBounds());
}

} // namespace vmp

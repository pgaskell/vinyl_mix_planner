#pragma once

#include "core/Set.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace vmp
{

// What validate() found. Clicking an issue selects its clip.
class IssuesPanel : public juce::Component, private juce::ListBoxModel
{
public:
    IssuesPanel();

    void setIssues(std::vector<Issue> issues);
    std::function<void(Id)> onIssueClicked;

    void resized() override { list_.setBounds(getLocalBounds()); }

private:
    int getNumRows() override { return std::max<int>(1, static_cast<int>(issues_.size())); }
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;

    const juce::Font rowFont_{juce::FontOptions(13.0f)};
    juce::ListBox list_{"Issues", this};
    std::vector<Issue> issues_;
};

} // namespace vmp

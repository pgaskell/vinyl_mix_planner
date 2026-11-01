#include "app/IssuesPanel.h"

namespace vmp
{

IssuesPanel::IssuesPanel()
{
    list_.setRowHeight(22);
    list_.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff23262b));
    addAndMakeVisible(list_);
}

void IssuesPanel::setIssues(std::vector<Issue> issues)
{
    issues_ = std::move(issues);
    list_.updateContent();
    list_.repaint();
}

void IssuesPanel::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (selected)
        g.fillAll(juce::Colour(0xff3a3e45));
    g.setFont(rowFont_);

    if (issues_.empty())
    {
        g.setColour(juce::Colours::grey);
        g.drawText("No issues: everything here can be played on real decks.", 8, 0, width - 16, height,
                   juce::Justification::centredLeft);
        return;
    }

    const auto& issue = issues_[static_cast<size_t>(row)];
    const bool error = issue.severity == Issue::Severity::Error;
    g.setColour(error ? juce::Colours::red : juce::Colours::orange);
    g.drawText(error ? "Error" : "Warning", 8, 0, 70, height, juce::Justification::centredLeft);
    g.setColour(juce::Colours::white);
    g.drawText(juce::CharPointer_UTF8(issue.message.c_str()), 80, 0, width - 88, height,
               juce::Justification::centredLeft, true);
}

void IssuesPanel::listBoxItemClicked(int row, const juce::MouseEvent&)
{
    if (row >= 0 && row < static_cast<int>(issues_.size()) && !issues_[static_cast<size_t>(row)].clipIds.empty() &&
        onIssueClicked)
        onIssueClicked(issues_[static_cast<size_t>(row)].clipIds.front());
}

} // namespace vmp

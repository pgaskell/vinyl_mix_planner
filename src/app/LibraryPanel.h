#pragma once

#include "app/AudioPool.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace vmp
{

// A folder tree of audio files. Drag files onto a deck lane, or double-click to add one at the
// playhead.
class LibraryPanel : public juce::Component, private juce::FileBrowserListener
{
public:
    LibraryPanel(AudioPool& pool, juce::PropertiesFile& settings);
    ~LibraryPanel() override;

    std::function<void(const juce::File&)> onFileDoubleClicked;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void setRoot(const juce::File& folder);

    void selectionChanged() override {}
    void fileClicked(const juce::File&, const juce::MouseEvent&) override {}
    void fileDoubleClicked(const juce::File& file) override;
    void browserRootChanged(const juce::File&) override {}

    juce::PropertiesFile& settings_;
    juce::TimeSliceThread scanner_{"Library scanner"};
    juce::WildcardFileFilter filter_;
    juce::DirectoryContentsList contents_;
    juce::FileTreeComponent tree_;
    juce::TextButton chooseButton_{"Choose folder..."};
    juce::Label rootLabel_;
    std::unique_ptr<juce::FileChooser> chooser_;
};

} // namespace vmp

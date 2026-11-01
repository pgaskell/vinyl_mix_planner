#pragma once

#include "app/AudioPool.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace vmp
{

// The folder tree. Items are as wide as their names, so long names can be scrolled to sideways.
// A folder's contents are listed when it's opened (folders first, then audio files, by name);
// close and reopen it to see changes.
class LibraryTree : public juce::TreeView
{
public:
    LibraryTree(juce::AudioFormatManager& formats);
    ~LibraryTree() override;

    void setRoot(const juce::File& folder);
    // Selected audio files, in display order.
    juce::Array<juce::File> selectedFiles() const;

    std::function<void(const juce::File&)> onFileDoubleClicked;

    bool isAudioFile(const juce::File& file) const;
    const juce::Font& font() const { return font_; }

private:
    juce::AudioFormatManager& formats_;
    const juce::Font font_{juce::FontOptions(13.0f)};
    std::unique_ptr<juce::TreeViewItem> root_;
};

// The library: a folder chooser and the tree. Drag files onto a deck lane, or double-click one to
// add it at the playhead.
class LibraryPanel : public juce::Component
{
public:
    LibraryPanel(AudioPool& pool, juce::PropertiesFile& settings);

    std::function<void(const juce::File&)> onFileDoubleClicked;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void setRoot(const juce::File& folder);

    juce::PropertiesFile& settings_;
    LibraryTree tree_;
    juce::TextButton chooseButton_{"Choose folder..."};
    juce::Label rootLabel_;
    std::unique_ptr<juce::FileChooser> chooser_;
};

} // namespace vmp

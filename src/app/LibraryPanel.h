#pragma once

#include "app/AudioPool.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

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
    const juce::File& folder() const { return folder_; }
    // Shows these files as a flat list, named by their path relative to the root, instead of the
    // folders. The folders come back, opened as they were, with showFolders().
    void showFiles(const juce::Array<juce::File>& files);
    void showFolders();
    // Selected audio files, in display order.
    juce::Array<juce::File> selectedFiles() const;

    std::function<void(const juce::File&)> onFileDoubleClicked;

    bool isAudioFile(const juce::File& file) const;
    const juce::Font& font() const { return font_; }

private:
    juce::AudioFormatManager& formats_;
    const juce::Font font_{juce::FontOptions(13.0f)};
    juce::File folder_;
    std::unique_ptr<juce::TreeViewItem> root_;
    std::unique_ptr<juce::XmlElement> folderOpenness_; // while showing files
};

// Every audio file under a folder, with its path relative to the folder, for searching.
struct LibraryIndex
{
    struct Entry
    {
        juce::File file;
        juce::String lowerPath; // relative, lower case
    };
    juce::File root;
    std::vector<Entry> entries; // sorted by path
};

// The library: a folder chooser, a search box and the tree. Drag files onto a deck lane, or
// double-click one to add it at the playhead.
// Searching lists the audio files whose path below the root contains every word typed. The folder
// is scanned on a background thread each time a search starts; until the first scan finishes there
// is nothing to search, after that the previous scan is searched while the next one runs.
class LibraryPanel : public juce::Component
{
public:
    LibraryPanel(AudioPool& pool, juce::PropertiesFile& settings);
    ~LibraryPanel() override;

    std::function<void(const juce::File&)> onFileDoubleClicked;
    // Return or Esc in the search box: hand the keyboard back.
    std::function<void()> onSearchDone;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void setRoot(const juce::File& folder);
    void startScan();
    void indexScanned(std::shared_ptr<const LibraryIndex> index);
    void updateResults();

    class Scanner;

    juce::PropertiesFile& settings_;
    LibraryTree tree_;
    juce::AudioFormatManager& formats_;
    juce::TextButton chooseButton_{"Choose folder..."};
    juce::Label rootLabel_;
    juce::TextEditor searchBox_;
    juce::Label searchStatus_;
    std::unique_ptr<Scanner> scanner_;
    std::shared_ptr<const LibraryIndex> index_;
    juce::String lastQuery_;
    std::unique_ptr<juce::FileChooser> chooser_;
};

} // namespace vmp

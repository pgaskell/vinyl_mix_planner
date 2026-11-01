#include "app/LibraryPanel.h"

namespace vmp
{

LibraryPanel::LibraryPanel(AudioPool& pool, juce::PropertiesFile& settings)
    : settings_(settings), filter_(pool.formats().getWildcardForAllFormats(), "*", "Audio files"),
      contents_(&filter_, scanner_), tree_(contents_)
{
    scanner_.startThread(juce::Thread::Priority::low);

    tree_.setMultiSelectEnabled(true);
    tree_.setDragAndDropDescription("audio-files"); // lets files be dragged onto the timeline
    tree_.addListener(this);
    tree_.setColour(juce::TreeView::backgroundColourId, juce::Colour(0xff23262b));
    addAndMakeVisible(tree_);

    chooseButton_.setWantsKeyboardFocus(false);
    chooseButton_.onClick = [this] {
        chooser_ = std::make_unique<juce::FileChooser>("Choose your music folder", contents_.getDirectory());
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this](const juce::FileChooser& fc) {
                                  if (fc.getResult().isDirectory())
                                      setRoot(fc.getResult());
                              });
    };
    addAndMakeVisible(chooseButton_);

    rootLabel_.setFont(juce::FontOptions(12.0f));
    rootLabel_.setMinimumHorizontalScale(0.5f);
    addAndMakeVisible(rootLabel_);

    const juce::File saved(settings_.getValue("libraryRoot"));
    setRoot(saved.isDirectory() ? saved : juce::File::getSpecialLocation(juce::File::userMusicDirectory));
}

LibraryPanel::~LibraryPanel()
{
    tree_.removeListener(this);
    scanner_.stopThread(2000);
}

void LibraryPanel::setRoot(const juce::File& folder)
{
    contents_.setDirectory(folder, true, true);
    rootLabel_.setText(folder.getFullPathName(), juce::dontSendNotification);
    settings_.setValue("libraryRoot", folder.getFullPathName());
}

void LibraryPanel::fileDoubleClicked(const juce::File& file)
{
    if (file.existsAsFile() && onFileDoubleClicked)
        onFileDoubleClicked(file);
}

void LibraryPanel::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff23262b));
}

void LibraryPanel::resized()
{
    auto area = getLocalBounds().reduced(6);
    chooseButton_.setBounds(area.removeFromTop(26));
    rootLabel_.setBounds(area.removeFromTop(20));
    area.removeFromTop(4);
    tree_.setBounds(area);
}

} // namespace vmp

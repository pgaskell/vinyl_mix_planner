#include "app/LibraryPanel.h"

namespace vmp
{

namespace
{
const juce::Colour kPanelColour{0xff23262b};

class FileItem final : public juce::TreeViewItem
{
public:
    FileItem(LibraryTree& tree, juce::File file) : tree_(tree), file_(std::move(file)), folder_(file_.isDirectory())
    {
        const auto name = file_.getFileName();
        width_ = kIconWidth + juce::roundToInt(juce::GlyphArrangement::getStringWidth(tree_.font(), name)) + 12;
    }

    const juce::File& file() const { return file_; }

    bool mightContainSubItems() override { return folder_; }
    int getItemWidth() const override { return width_; }
    int getItemHeight() const override { return 22; }
    juce::String getUniqueName() const override { return file_.getFullPathName(); }
    bool canBeSelected() const override { return true; }

    void itemOpennessChanged(bool isNowOpen) override
    {
        if (!isNowOpen)
        {
            clearSubItems(); // listed afresh next time, so changes on disk show up
            return;
        }
        juce::Array<juce::File> folders, files;
        for (const auto& entry : juce::RangedDirectoryIterator(file_, false, "*",
                                                               juce::File::findFilesAndDirectories |
                                                                   juce::File::ignoreHiddenFiles))
        {
            const auto& child = entry.getFile();
            if (entry.isDirectory())
                folders.add(child);
            else if (tree_.isAudioFile(child))
                files.add(child);
        }
        auto byName = [](juce::Array<juce::File>& list) {
            std::sort(list.begin(), list.end(), [](const juce::File& a, const juce::File& b) {
                return a.getFileName().compareNatural(b.getFileName()) < 0;
            });
        };
        byName(folders);
        byName(files);
        for (const auto& f : folders)
            addSubItem(new FileItem(tree_, f));
        for (const auto& f : files)
            addSubItem(new FileItem(tree_, f));
    }

    void paintItem(juce::Graphics& g, int width, int height) override
    {
        if (isSelected())
            g.fillAll(juce::Colour(0xff3a4a66));
        auto area = juce::Rectangle<int>(0, 0, width, height);
        auto icon = area.removeFromLeft(kIconWidth).toFloat().reduced(3.0f, 5.0f);
        if (folder_)
        {
            g.setColour(juce::Colour(0xffc8a24a));
            g.fillRoundedRectangle(icon.withTrimmedTop(2.0f), 1.5f);
            g.fillRoundedRectangle(icon.withWidth(icon.getWidth() * 0.45f).withHeight(4.0f), 1.0f);
        }
        else
        {
            // A small record.
            const auto disc = icon.withSizeKeepingCentre(icon.getHeight(), icon.getHeight());
            g.setColour(juce::Colour(0xff8a8f99));
            g.fillEllipse(disc);
            g.setColour(kPanelColour);
            g.fillEllipse(disc.withSizeKeepingCentre(3.0f, 3.0f));
        }
        g.setColour(juce::Colours::white.withAlpha(folder_ ? 1.0f : 0.85f));
        g.setFont(tree_.font());
        g.drawText(file_.getFileName(), area.withTrimmedLeft(2), juce::Justification::centredLeft, false);
    }

    void itemDoubleClicked(const juce::MouseEvent&) override
    {
        if (folder_)
            setOpen(!isOpen());
        else if (tree_.onFileDoubleClicked)
            tree_.onFileDoubleClicked(file_);
    }

    // Lets files be dragged onto the timeline.
    juce::var getDragSourceDescription() override { return folder_ ? juce::var() : juce::var("audio-files"); }

private:
    static constexpr int kIconWidth = 20;
    LibraryTree& tree_;
    juce::File file_;
    bool folder_;
    int width_;
};

void collectSelected(juce::TreeViewItem& item, juce::Array<juce::File>& files, const LibraryTree& tree)
{
    for (int i = 0; i < item.getNumSubItems(); ++i)
    {
        auto* child = static_cast<FileItem*>(item.getSubItem(i));
        if (child->isSelected() && tree.isAudioFile(child->file()))
            files.add(child->file());
        collectSelected(*child, files, tree);
    }
}
} // namespace

// --- LibraryTree ------------------------------------------------------------

LibraryTree::LibraryTree(juce::AudioFormatManager& formats) : formats_(formats)
{
    setMultiSelectEnabled(true);
    setRootItemVisible(false);
    setDefaultOpenness(false);
    setColour(juce::TreeView::backgroundColourId, kPanelColour);
    setWantsKeyboardFocus(false); // leave the arrow keys and space to the app's shortcuts
}

LibraryTree::~LibraryTree()
{
    setRootItem(nullptr);
}

void LibraryTree::setRoot(const juce::File& folder)
{
    setRootItem(nullptr);
    root_ = std::make_unique<FileItem>(*this, folder);
    setRootItem(root_.get());
    root_->setOpen(true);
}

bool LibraryTree::isAudioFile(const juce::File& file) const
{
    return file.existsAsFile() && formats_.findFormatForFileExtension(file.getFileExtension()) != nullptr;
}

juce::Array<juce::File> LibraryTree::selectedFiles() const
{
    juce::Array<juce::File> files;
    if (root_ != nullptr)
        collectSelected(*root_, files, *this);
    return files;
}

// --- LibraryPanel -----------------------------------------------------------

LibraryPanel::LibraryPanel(AudioPool& pool, juce::PropertiesFile& settings)
    : settings_(settings), tree_(pool.formats())
{
    tree_.onFileDoubleClicked = [this](const juce::File& file) {
        if (onFileDoubleClicked)
            onFileDoubleClicked(file);
    };
    addAndMakeVisible(tree_);

    chooseButton_.setWantsKeyboardFocus(false);
    chooseButton_.onClick = [this] {
        chooser_ = std::make_unique<juce::FileChooser>("Choose your music folder",
                                                       juce::File(settings_.getValue("libraryRoot")));
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

void LibraryPanel::setRoot(const juce::File& folder)
{
    tree_.setRoot(folder);
    rootLabel_.setText(folder.getFullPathName(), juce::dontSendNotification);
    settings_.setValue("libraryRoot", folder.getFullPathName());
}

void LibraryPanel::paint(juce::Graphics& g)
{
    g.fillAll(kPanelColour);
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

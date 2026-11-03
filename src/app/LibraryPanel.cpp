#include "app/LibraryPanel.h"

namespace vmp
{

namespace
{
const juce::Colour kPanelColour{0xff23262b};
constexpr int kMaxResults = 1000; // more are pointless to scroll through, and slow to list

class FileItem final : public juce::TreeViewItem
{
public:
    FileItem(LibraryTree& tree, juce::File file) : FileItem(tree, file, file.getFileName(), file.isDirectory()) {}
    FileItem(LibraryTree& tree, juce::File file, juce::String name, bool folder)
        : tree_(tree), file_(std::move(file)), name_(std::move(name)), folder_(folder)
    {
        width_ = kIconWidth + juce::roundToInt(juce::GlyphArrangement::getStringWidth(tree_.font(), name_)) + 12;
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
        g.drawText(name_, area.withTrimmedLeft(2), juce::Justification::centredLeft, false);
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
    juce::String name_;
    bool folder_;
    int width_;
};

// The hidden root of a search's results.
class ResultsItem final : public juce::TreeViewItem
{
public:
    bool mightContainSubItems() override { return true; }
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
    folder_ = folder;
    folderOpenness_.reset();
    setRootItem(nullptr);
    root_ = std::make_unique<FileItem>(*this, folder);
    setRootItem(root_.get());
    root_->setOpen(true);
}

void LibraryTree::showFiles(const juce::Array<juce::File>& files)
{
    if (folderOpenness_ == nullptr)
        folderOpenness_ = getOpennessState(true);
    setRootItem(nullptr);
    root_ = std::make_unique<ResultsItem>();
    for (const auto& file : files)
        root_->addSubItem(new FileItem(*this, file, file.getRelativePathFrom(folder_), false));
    setRootItem(root_.get());
    root_->setOpen(true);
}

void LibraryTree::showFolders()
{
    if (folderOpenness_ == nullptr)
        return; // already showing them
    const auto openness = std::move(folderOpenness_);
    setRoot(folder_);
    restoreOpennessState(*openness, true);
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

class LibraryPanel::Scanner final : public juce::Thread
{
public:
    Scanner(LibraryPanel& owner, juce::File root, juce::String wildcard)
        : juce::Thread("Library scan"), owner_(&owner), root_(std::move(root)), wildcard_(std::move(wildcard))
    {
        startThread();
    }
    ~Scanner() override { stopThread(10000); }

    const juce::File& root() const { return root_; }

private:
    void run() override
    {
        auto index = std::make_shared<LibraryIndex>();
        index->root = root_;
        for (const auto& entry : juce::RangedDirectoryIterator(root_, true, wildcard_,
                                                               juce::File::findFiles | juce::File::ignoreHiddenFiles))
        {
            if (threadShouldExit())
                return;
            const auto& file = entry.getFile();
            index->entries.push_back({file, file.getRelativePathFrom(root_).toLowerCase()});
        }
        std::sort(index->entries.begin(), index->entries.end(), [](const auto& a, const auto& b) {
            return a.lowerPath.compareNatural(b.lowerPath) < 0;
        });
        juce::MessageManager::callAsync([owner = owner_, index = std::shared_ptr<const LibraryIndex>(std::move(index))] {
            if (owner != nullptr)
                owner->indexScanned(index);
        });
    }

    juce::Component::SafePointer<LibraryPanel> owner_;
    juce::File root_;
    juce::String wildcard_;
};

LibraryPanel::LibraryPanel(AudioPool& pool, juce::PropertiesFile& settings)
    : settings_(settings), tree_(pool.formats()), formats_(pool.formats())
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

    searchBox_.setTextToShowWhenEmpty("Search", juce::Colours::grey);
    searchBox_.setFont(juce::FontOptions(13.0f));
    searchBox_.onTextChange = [this] { updateResults(); };
    searchBox_.onReturnKey = [this] {
        if (onSearchDone)
            onSearchDone();
    };
    searchBox_.onEscapeKey = [this] {
        searchBox_.setText({}, false);
        updateResults();
        if (onSearchDone)
            onSearchDone();
    };
    addAndMakeVisible(searchBox_);

    searchStatus_.setFont(juce::FontOptions(11.0f));
    searchStatus_.setColour(juce::Label::textColourId, juce::Colours::grey);
    addChildComponent(searchStatus_);

    const juce::File saved(settings_.getValue("libraryRoot"));
    setRoot(saved.isDirectory() ? saved : juce::File::getSpecialLocation(juce::File::userMusicDirectory));
}

LibraryPanel::~LibraryPanel() = default;

void LibraryPanel::setRoot(const juce::File& folder)
{
    scanner_.reset();
    index_.reset();
    searchBox_.setText({}, false);
    updateResults();
    tree_.setRoot(folder);
    rootLabel_.setText(folder.getFullPathName(), juce::dontSendNotification);
    settings_.setValue("libraryRoot", folder.getFullPathName());
}

void LibraryPanel::startScan()
{
    if (scanner_ != nullptr && scanner_->isThreadRunning() && scanner_->root() == tree_.folder())
        return;
    scanner_.reset();
    scanner_ = std::make_unique<Scanner>(*this, tree_.folder(), formats_.getWildcardForAllFormats());
}

void LibraryPanel::indexScanned(std::shared_ptr<const LibraryIndex> index)
{
    if (index->root != tree_.folder())
        return;
    const auto unchanged = index_ != nullptr && index_->entries.size() == index->entries.size() &&
                           std::equal(index->entries.begin(), index->entries.end(), index_->entries.begin(),
                                      [](const auto& a, const auto& b) { return a.file == b.file; });
    index_ = std::move(index);
    if (!unchanged) // rebuilding the list would lose the selection
        updateResults();
}

void LibraryPanel::updateResults()
{
    const auto query = searchBox_.getText().trim();
    const auto starting = lastQuery_.isEmpty();
    lastQuery_ = query;
    if (query.isEmpty())
    {
        tree_.showFolders();
        if (searchStatus_.isVisible())
        {
            searchStatus_.setVisible(false);
            resized();
        }
        return;
    }
    if (starting)
        startScan(); // picks up changes on disk since the last search
    if (!searchStatus_.isVisible())
    {
        searchStatus_.setVisible(true);
        resized();
    }
    if (index_ == nullptr)
    {
        tree_.showFiles({});
        searchStatus_.setText("Scanning folder...", juce::dontSendNotification);
        return;
    }

    auto words = juce::StringArray::fromTokens(query.toLowerCase(), true);
    words.removeEmptyStrings();
    juce::Array<juce::File> matches;
    int count = 0;
    for (const auto& entry : index_->entries)
    {
        if (std::all_of(words.begin(), words.end(), [&](const auto& w) { return entry.lowerPath.contains(w); }))
        {
            if (++count <= kMaxResults)
                matches.add(entry.file);
        }
    }
    tree_.showFiles(matches);
    searchStatus_.setText(count == 0                ? juce::String("No matches")
                          : count == 1              ? juce::String("1 file")
                          : count <= kMaxResults ? juce::String(count) + " files"
                                                    : "First " + juce::String(kMaxResults) + " of " +
                                                          juce::String(count) + " files",
                          juce::dontSendNotification);
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
    area.removeFromTop(2);
    searchBox_.setBounds(area.removeFromTop(24));
    if (searchStatus_.isVisible())
        searchStatus_.setBounds(area.removeFromTop(18));
    area.removeFromTop(4);
    tree_.setBounds(area);
}

} // namespace vmp

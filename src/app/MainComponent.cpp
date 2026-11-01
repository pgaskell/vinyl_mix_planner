#include "app/MainComponent.h"

#include "app/TimeFormat.h"
#include "core/Edits.h"
#include "core/SetFile.h"

namespace vmp
{

namespace
{
enum Command : juce::CommandID
{
    kNew = 1,
    kOpen,
    kSave,
    kSaveAs,
    kUndo,
    kRedo,
    kDelete,
    kPlayPause,
    kGoToStart,
    kZoomIn,
    kZoomOut,
    kZoomSet,
    kZoomClip,
    kZoomTransition,
    kToggleFollow,
    kNudgeEarlier,
    kNudgeLater,
    kNudgeEarlierFine,
    kNudgeLaterFine,
    kPitchUp,
    kPitchDown,
    kAudioSettings,
};

// Menu item ids outside the command range.
constexpr int kMenuModeInternal = 1001;
constexpr int kMenuModeDirect = 1002;
constexpr int kMenuMasterBase = 2000; // + first channel; kMenuMasterBase + 999 is "None"
constexpr int kMenuDeckBase = 3000;   // + deck * 100 + first channel; + 99 is "None"
constexpr int kMenuDecksBase = 4000;  // + number of decks
constexpr int kMenuNone = 99;

constexpr double kNudgeSeconds = 0.010;
constexpr double kFineNudgeSeconds = 0.001;
constexpr int kMidiLogLines = 300;
const juce::Colour kBackground{0xff1b1d21};
} // namespace

MainComponent::MainComponent(juce::PropertiesFile& settings)
    : settings_(settings), timeline_(document_, pool_, engine_), inspector_(document_), library_(pool_, settings)
{
    // --- Commands, menus and shortcuts ---
    commands_.registerAllCommandsForTarget(this);
    commands_.setFirstCommandTarget(this);
    addKeyListener(commands_.getKeyMappings());
    setApplicationCommandManagerToWatch(&commands_);
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(this);
#endif

    // --- Panels ---
    addAndMakeVisible(timeline_);
    timeline_.onSelectionChanged = [this](Id id) {
        inspector_.setClip(id);
        commands_.commandStatusChanged();
    };
    timeline_.onFileDropped = [this](const juce::File& file, int deck, double start) {
        return addFile(file, deck, start);
    };

    addAndMakeVisible(inspector_);
    addAndMakeVisible(library_);
    library_.onFileDoubleClicked = [this](const juce::File& file) { addFileAtPlayhead(file); };

    issues_.onIssueClicked = [this](Id clip) {
        timeline_.setSelectedClip(clip);
        timeline_.zoomToClip();
    };
    midiLog_.setMultiLine(true);
    midiLog_.setReadOnly(true);
    midiLog_.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
    const auto tabColour = juce::Colour(0xff23262b);
    bottomTabs_.addTab("Issues", tabColour, &issues_, false);
    bottomTabs_.addTab("MIDI monitor", tabColour, &midiLog_, false);
    addAndMakeVisible(bottomTabs_);

    // --- Toolbar ---
    auto commandButton = [this](juce::Button& button, Command command, const juce::String& tooltip) {
        button.setWantsKeyboardFocus(false);
        button.setTooltip(tooltip);
        button.onClick = [this, command] { commands_.invokeDirectly(command, true); };
        addAndMakeVisible(button);
    };
    commandButton(playButton_, kPlayPause, "Play / pause (space)");
    commandButton(zoomOutButton_, kZoomOut, "Zoom out (cmd -)");
    commandButton(zoomInButton_, kZoomIn, "Zoom in (cmd =)");
    commandButton(zoomSetButton_, kZoomSet, "Zoom to the whole mix (shift Z)");
    commandButton(zoomClipButton_, kZoomClip, "Zoom to the selected clip (Z)");
    commandButton(zoomTransitionButton_, kZoomTransition, "Zoom to the mix into the selected clip (T)");
    commandButton(followButton_, kToggleFollow, "Keep the playhead in view while playing (F)");
    commandButton(settingsButton_, kAudioSettings, "Audio and MIDI device settings");
    followButton_.setToggleState(true, juce::dontSendNotification);

    timeLabel_.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 16.0f, juce::Font::plain));
    addAndMakeVisible(timeLabel_);

    modeBox_.addItem("Internal mix (laptop)", 1);
    modeBox_.addItem("Direct out (hardware mixer)", 2);
    modeBox_.setWantsKeyboardFocus(false);
    modeBox_.onChange = [this] {
        setOutputMode(modeBox_.getSelectedId() == 2 ? OutputMode::DirectOut : OutputMode::InternalMix);
    };
    addAndMakeVisible(modeBox_);

    for (int n = 2; n <= kMaxDecks; ++n)
        decksBox_.addItem(juce::String(n) + " decks", n);
    decksBox_.setWantsKeyboardFocus(false);
    decksBox_.onChange = [this] { menuItemSelected(kMenuDecksBase + decksBox_.getSelectedId(), 3); };
    addAndMakeVisible(decksBox_);

    deviceLabel_.setFont(juce::FontOptions(12.0f));
    deviceLabel_.setMinimumHorizontalScale(0.6f);
    addAndMakeVisible(deviceLabel_);

    // --- Audio and MIDI ---
    restoreRouting();
    const auto savedDevice = settings_.getXmlValue("audioDeviceState");
    const auto error = deviceManager_.initialise(0, 2, savedDevice.get(), true);
    if (savedDevice == nullptr)
        for (const auto& input : juce::MidiInput::getAvailableDevices())
            deviceManager_.setMidiInputDeviceEnabled(input.identifier, true);
    deviceManager_.addAudioCallback(this);
    deviceManager_.addMidiInputDeviceCallback({}, this);
    deviceManager_.addChangeListener(this);
    pool_.addChangeListener(this);
    updateDeviceInfo();
    if (error.isNotEmpty())
        deviceLabel_.setText("Audio device error: " + error, juce::dontSendNotification);

    setSize(1400, 860);

    // --- Document: reopen the last set ---
    const juce::File lastSet(settings_.getValue("lastSet"));
    if (lastSet.existsAsFile())
        openSetFile(lastSet);
    else
        document_.reset(Set{});
    documentChanged();
    timeline_.zoomToSet();

    startTimerHz(30);
}

MainComponent::~MainComponent()
{
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(nullptr);
#endif
    saveRouting();
    pool_.removeChangeListener(this);
    deviceManager_.removeChangeListener(this);
    deviceManager_.removeMidiInputDeviceCallback({}, this);
    deviceManager_.removeAudioCallback(this);
    deviceManager_.closeAudioDevice();
}

// --- Audio and MIDI ---------------------------------------------------------

void MainComponent::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    // The driver hands us only the enabled channels, packed together. Map physical -> buffer index.
    const auto active = device->getActiveOutputChannels();
    const int physical = device->getOutputChannelNames().size();
    std::vector<int> physicalToBuffer(static_cast<size_t>(physical), -1);
    int next = 0;
    for (int ch = 0; ch < physical; ++ch)
        if (active[ch])
            physicalToBuffer[static_cast<size_t>(ch)] = next++;

    engine_.prepare(device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples(),
                    std::move(physicalToBuffer));
}

void MainComponent::audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs,
                                                     int numOutputs, int numSamples,
                                                     const juce::AudioIODeviceCallbackContext&)
{
    engine_.render(outputs, numOutputs, numSamples);
}

void MainComponent::handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message)
{
    // Clock and active sensing arrive constantly and would drown out the controls.
    if (message.isMidiClock() || message.isActiveSense())
        return;

    const auto line = juce::Time::getCurrentTime().formatted("%H:%M:%S") + "  " +
                      (source != nullptr ? source->getName() : juce::String("?")) + "  " +
                      juce::String::toHexString(message.getRawData(), message.getRawDataSize()) + "  " +
                      message.getDescription();
    const juce::ScopedLock lock(midiLock_);
    pendingMidi_.add(line);
}

// --- Updates ----------------------------------------------------------------

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &pool_)
    {
        timeline_.refresh();
        rebuildSchedule();
        return;
    }

    if (auto state = deviceManager_.createStateXml())
        settings_.setValue("audioDeviceState", state.get());
    updateDeviceInfo();
}

void MainComponent::timerCallback()
{
    if (document_.revision() != seenRevision_)
        documentChanged();

    timeline_.updatePlayhead();
    timeLabel_.setText(formatTime(engine_.playheadSeconds(), true), juce::dontSendNotification);
    playButton_.setButtonText(engine_.isPlaying() ? "Pause" : "Play");
    engine_.collectGarbage();

    juce::StringArray incoming;
    {
        const juce::ScopedLock lock(midiLock_);
        incoming.swapWith(pendingMidi_);
    }
    if (!incoming.isEmpty())
    {
        midiLines_.addArray(incoming);
        if (midiLines_.size() > kMidiLogLines)
            midiLines_.removeRange(0, midiLines_.size() - kMidiLogLines);
        midiLog_.setText(midiLines_.joinIntoString("\n"), false);
        midiLog_.moveCaretToEnd();
    }
}

void MainComponent::documentChanged()
{
    seenRevision_ = document_.revision();
    const Set& set = document_.set();
    for (const auto& track : set.tracks)
        pool_.request(track.path);

    timeline_.refresh();
    inspector_.refresh();
    issues_.setIssues(validate(set));
    decksBox_.setSelectedId(set.numDecks, juce::dontSendNotification);
    rebuildSchedule();
    updateTitle();
    commands_.commandStatusChanged();
}

void MainComponent::rebuildSchedule()
{
    engine_.setSchedule(buildSchedule(document_.set(), [this](const Track& track) {
        const auto* entry = pool_.find(track.path);
        return entry != nullptr && entry->status == AudioPool::Status::Ready ? entry->audio : nullptr;
    }));
}

void MainComponent::updateTitle()
{
    const juce::String name = file_ == juce::File() ? "Untitled" : file_.getFileNameWithoutExtension();
    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
        window->setName(name + (document_.isDirty() ? " *" : "") + " - Vinyl Mix Planner");
}

std::vector<MainComponent::OutputPair> MainComponent::outputPairs()
{
    std::vector<OutputPair> pairs;
    if (auto* device = deviceManager_.getCurrentAudioDevice())
    {
        const auto names = device->getOutputChannelNames();
        const auto active = device->getActiveOutputChannels();
        for (int ch = 0; ch + 1 < names.size(); ch += 2)
            pairs.push_back({ch, juce::String(ch + 1) + "/" + juce::String(ch + 2), active[ch] && active[ch + 1]});
    }
    return pairs;
}

void MainComponent::updateDeviceInfo()
{
    if (auto* device = deviceManager_.getCurrentAudioDevice())
    {
        const auto active = device->getActiveOutputChannels();
        deviceLabel_.setText(device->getName() + "  " + juce::String(device->getCurrentSampleRate() / 1000.0, 1) +
                                 " kHz, " + juce::String(device->getCurrentBufferSizeSamples()) + " samples, " +
                                 juce::String(active.countNumberOfSetBits()) + " of " +
                                 juce::String(device->getOutputChannelNames().size()) + " outputs on",
                             juce::dontSendNotification);
    }
    else
    {
        deviceLabel_.setText("No audio device", juce::dontSendNotification);
    }
    menuItemsChanged();
}

// --- Editing ----------------------------------------------------------------

std::optional<double> MainComponent::addFile(const juce::File& file, int deck, double start)
{
    const auto track = pool_.probe(file);
    if (!track)
    {
        showError("Can't add " + file.getFileName(), "The file is not a supported audio format, or is unreadable.");
        return std::nullopt;
    }

    Id clipId = 0;
    document_.edit([&](Set& s) {
        clipId = edit::addClip(s, edit::addTrack(s, *track), deck, start);
        return clipId != 0;
    });
    if (clipId == 0)
        return std::nullopt;

    documentChanged();
    timeline_.setSelectedClip(clipId);

    const Set& set = document_.set();
    const Clip* clip = set.findClip(clipId);
    return ClipTiming(*clip, *set.findTrack(clip->trackId)).endSeconds();
}

void MainComponent::addFileAtPlayhead(const juce::File& file)
{
    const Clip* selected = document_.set().findClip(timeline_.selectedClip());
    addFile(file, selected != nullptr ? selected->deck : 0, engine_.playheadSeconds());
}

void MainComponent::nudgeSelected(double seconds)
{
    const Clip* clip = document_.set().findClip(timeline_.selectedClip());
    if (clip == nullptr)
        return;
    const Id id = clip->id;
    const double start = clip->startSeconds + seconds;
    const int deck = clip->deck;
    document_.edit([&](Set& s) { return edit::moveClip(s, id, start, deck); });
}

void MainComponent::changeSelectedPitch(int steps)
{
    const Clip* clip = document_.set().findClip(timeline_.selectedClip());
    if (clip == nullptr)
        return;
    const Id id = clip->id;
    const int pitch = clip->pitchSteps + steps;
    document_.edit([&](Set& s) { return edit::setClipPitch(s, id, pitch); });
}

// --- Files ------------------------------------------------------------------

void MainComponent::confirmDiscard(std::function<void()> then)
{
    if (!document_.isDirty())
    {
        then();
        return;
    }
    const juce::String name = file_ == juce::File() ? "this set" : "\"" + file_.getFileNameWithoutExtension() + "\"";
    const auto options = juce::MessageBoxOptions()
                             .withIconType(juce::MessageBoxIconType::QuestionIcon)
                             .withTitle("Save changes?")
                             .withMessage("Do you want to save the changes to " + name + "?")
                             .withButton("Save")
                             .withButton("Don't Save")
                             .withButton("Cancel");
    // Result codes: Save = 1, Don't Save = 2, Cancel = 0.
    messageBox_ = juce::NativeMessageBox::showScopedAsync(options, [this, then](int result) {
        if (result == 1)
            save(false, then);
        else if (result == 2)
            then();
    });
}

void MainComponent::newSet()
{
    confirmDiscard([this] {
        document_.reset(Set{});
        file_ = juce::File();
        pool_.retainOnly({});
        documentChanged();
        timeline_.zoomToSet();
    });
}

void MainComponent::openSet()
{
    confirmDiscard([this] {
        chooser_ = std::make_unique<juce::FileChooser>("Open a set", file_.getParentDirectory(),
                                                       juce::String("*") + kSetFileExtension);
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this](const juce::FileChooser& fc) {
                                  if (fc.getResult().existsAsFile())
                                      openSetFile(fc.getResult());
                              });
    });
}

void MainComponent::openSetFile(const juce::File& file)
{
    try
    {
        document_.reset(loadSet(std::filesystem::path(file.getFullPathName().toStdString())));
    }
    catch (const SetFileError& e)
    {
        showError("Couldn't open " + file.getFileName(), e.what());
        return;
    }
    file_ = file;
    settings_.setValue("lastSet", file.getFullPathName());

    std::set<std::string> paths;
    for (const auto& track : document_.set().tracks)
        paths.insert(track.path);
    pool_.retainOnly(paths);
    timeline_.setSelectedClip(0);
    documentChanged();
    timeline_.zoomToSet();
}

void MainComponent::save(bool chooseFile, std::function<void()> onSaved)
{
    if (!chooseFile && file_ != juce::File())
    {
        if (writeSet(file_) && onSaved)
            onSaved();
        return;
    }

    const auto initial = file_ != juce::File()
                             ? file_
                             : juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                                   .getChildFile(juce::String("Untitled") + kSetFileExtension);
    chooser_ = std::make_unique<juce::FileChooser>("Save the set", initial, juce::String("*") + kSetFileExtension);
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                              juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, onSaved](const juce::FileChooser& fc) {
                              if (fc.getResult() == juce::File())
                                  return;
                              const auto file = fc.getResult().withFileExtension(kSetFileExtension);
                              if (!writeSet(file))
                                  return;
                              file_ = file;
                              settings_.setValue("lastSet", file.getFullPathName());
                              updateTitle();
                              if (onSaved)
                                  onSaved();
                          });
}

bool MainComponent::writeSet(const juce::File& file)
{
    Set set = document_.set();
    set.name = file.getFileNameWithoutExtension().toStdString();
    try
    {
        saveSet(set, std::filesystem::path(file.getFullPathName().toStdString()));
    }
    catch (const SetFileError& e)
    {
        showError("Couldn't save " + file.getFileName(), e.what());
        return false;
    }
    document_.markSaved();
    updateTitle();
    return true;
}

void MainComponent::showError(const juce::String& title, const juce::String& message)
{
    messageBox_ = juce::NativeMessageBox::showScopedAsync(juce::MessageBoxOptions()
                                                              .withIconType(juce::MessageBoxIconType::WarningIcon)
                                                              .withTitle(title)
                                                              .withMessage(message)
                                                              .withButton("OK"),
                                                          nullptr);
}

// --- Routing ----------------------------------------------------------------

void MainComponent::setOutputMode(OutputMode mode)
{
    engine_.setOutputMode(mode);
    modeBox_.setSelectedId(mode == OutputMode::DirectOut ? 2 : 1, juce::dontSendNotification);
    saveRouting();
    menuItemsChanged();
}

void MainComponent::restoreRouting()
{
    // Read everything before setOutputMode(), which saves all the routing settings.
    const bool direct = settings_.getIntValue("outputMode", 0) == 1;
    engine_.setMasterOutput(settings_.getIntValue("masterOutput", 0));
    for (int i = 0; i < kMaxDecks; ++i)
        engine_.setDeckOutput(i, settings_.getIntValue("deckOutput" + juce::String(i), i * 2));
    setOutputMode(direct ? OutputMode::DirectOut : OutputMode::InternalMix);
}

void MainComponent::saveRouting()
{
    settings_.setValue("outputMode", engine_.outputMode() == OutputMode::DirectOut ? 1 : 0);
    settings_.setValue("masterOutput", engine_.masterOutput());
    for (int i = 0; i < kMaxDecks; ++i)
        settings_.setValue("deckOutput" + juce::String(i), engine_.deckOutput(i));
}

// --- Commands ---------------------------------------------------------------

void MainComponent::getAllCommands(juce::Array<juce::CommandID>& commands)
{
    for (juce::CommandID id = kNew; id <= kAudioSettings; ++id)
        commands.add(id);
}

void MainComponent::getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info)
{
    using juce::KeyPress;
    using juce::ModifierKeys;
    const auto cmd = ModifierKeys::commandModifier;
    const auto shift = ModifierKeys::shiftModifier;
    const bool hasClip = document_.set().findClip(timeline_.selectedClip()) != nullptr;

    switch (id)
    {
    case kNew:
        info.setInfo("New Set", "Start a new set", "File", 0);
        info.addDefaultKeypress('n', cmd);
        break;
    case kOpen:
        info.setInfo("Open...", "Open a set", "File", 0);
        info.addDefaultKeypress('o', cmd);
        break;
    case kSave:
        info.setInfo("Save", "Save the set", "File", 0);
        info.addDefaultKeypress('s', cmd);
        break;
    case kSaveAs:
        info.setInfo("Save As...", "Save the set to a new file", "File", 0);
        info.addDefaultKeypress('s', cmd | shift);
        break;
    case kUndo:
        info.setInfo("Undo", "Undo the last change", "Edit", 0);
        info.addDefaultKeypress('z', cmd);
        info.setActive(document_.canUndo());
        break;
    case kRedo:
        info.setInfo("Redo", "Redo the last undone change", "Edit", 0);
        info.addDefaultKeypress('z', cmd | shift);
        info.setActive(document_.canRedo());
        break;
    case kDelete:
        info.setInfo("Delete Clip", "Remove the selected clip", "Edit", 0);
        info.addDefaultKeypress(KeyPress::backspaceKey, 0);
        info.addDefaultKeypress(KeyPress::deleteKey, 0);
        info.setActive(hasClip);
        break;
    case kNudgeEarlier:
        info.setInfo("Nudge Earlier", "Move the selected clip 10 ms earlier", "Edit", 0);
        info.addDefaultKeypress(KeyPress::leftKey, 0);
        info.setActive(hasClip);
        break;
    case kNudgeLater:
        info.setInfo("Nudge Later", "Move the selected clip 10 ms later", "Edit", 0);
        info.addDefaultKeypress(KeyPress::rightKey, 0);
        info.setActive(hasClip);
        break;
    case kNudgeEarlierFine:
        info.setInfo("Nudge Earlier (Fine)", "Move the selected clip 1 ms earlier", "Edit", 0);
        info.addDefaultKeypress(KeyPress::leftKey, shift);
        info.setActive(hasClip);
        break;
    case kNudgeLaterFine:
        info.setInfo("Nudge Later (Fine)", "Move the selected clip 1 ms later", "Edit", 0);
        info.addDefaultKeypress(KeyPress::rightKey, shift);
        info.setActive(hasClip);
        break;
    case kPitchUp:
        info.setInfo("Pitch +0.05%", "Speed up the selected clip by one step", "Edit", 0);
        info.addDefaultKeypress(KeyPress::upKey, 0);
        info.setActive(hasClip);
        break;
    case kPitchDown:
        info.setInfo("Pitch -0.05%", "Slow down the selected clip by one step", "Edit", 0);
        info.addDefaultKeypress(KeyPress::downKey, 0);
        info.setActive(hasClip);
        break;
    case kPlayPause:
        info.setInfo("Play / Pause", "Start or stop playback", "Transport", 0);
        info.addDefaultKeypress(KeyPress::spaceKey, 0);
        break;
    case kGoToStart:
        info.setInfo("Go to Start", "Move the playhead to the start", "Transport", 0);
        info.addDefaultKeypress(KeyPress::homeKey, 0);
        info.addDefaultKeypress(KeyPress::returnKey, 0);
        break;
    case kZoomIn:
        info.setInfo("Zoom In", "Zoom in around the playhead", "View", 0);
        info.addDefaultKeypress('=', cmd);
        break;
    case kZoomOut:
        info.setInfo("Zoom Out", "Zoom out", "View", 0);
        info.addDefaultKeypress('-', cmd);
        break;
    case kZoomSet:
        info.setInfo("Zoom to Whole Mix", "Fit the whole set in view", "View", 0);
        info.addDefaultKeypress('z', shift);
        break;
    case kZoomClip:
        info.setInfo("Zoom to Clip", "Fit the selected clip in view", "View", 0);
        info.addDefaultKeypress('z', 0);
        break;
    case kZoomTransition:
        info.setInfo("Zoom to Transition", "Fit the mix into the selected clip in view", "View", 0);
        info.addDefaultKeypress('t', 0);
        info.setActive(hasClip);
        break;
    case kToggleFollow:
        info.setInfo("Follow Playhead", "Keep the playhead in view while playing", "View", 0);
        info.addDefaultKeypress('f', 0);
        info.setTicked(timeline_.followsPlayhead());
        break;
    case kAudioSettings:
        info.setInfo("Audio / MIDI Settings...", "Choose audio and MIDI devices", "Audio", 0);
        info.addDefaultKeypress(',', cmd);
        break;
    default:
        break;
    }
}

bool MainComponent::perform(const InvocationInfo& info)
{
    switch (info.commandID)
    {
    case kNew:
        newSet();
        break;
    case kOpen:
        openSet();
        break;
    case kSave:
        save(false);
        break;
    case kSaveAs:
        save(true);
        break;
    case kUndo:
        document_.undo();
        break;
    case kRedo:
        document_.redo();
        break;
    case kDelete:
    {
        const Id id = timeline_.selectedClip();
        document_.edit([&](Set& s) { return edit::deleteClip(s, id); });
        break;
    }
    case kNudgeEarlier:
        nudgeSelected(-kNudgeSeconds);
        break;
    case kNudgeLater:
        nudgeSelected(kNudgeSeconds);
        break;
    case kNudgeEarlierFine:
        nudgeSelected(-kFineNudgeSeconds);
        break;
    case kNudgeLaterFine:
        nudgeSelected(kFineNudgeSeconds);
        break;
    case kPitchUp:
        changeSelectedPitch(1);
        break;
    case kPitchDown:
        changeSelectedPitch(-1);
        break;
    case kPlayPause:
        engine_.isPlaying() ? engine_.pause() : engine_.play();
        break;
    case kGoToStart:
        engine_.seek(0.0);
        break;
    case kZoomIn:
        timeline_.zoomIn();
        break;
    case kZoomOut:
        timeline_.zoomOut();
        break;
    case kZoomSet:
        timeline_.zoomToSet();
        break;
    case kZoomClip:
        timeline_.zoomToClip();
        break;
    case kZoomTransition:
        timeline_.zoomToTransition();
        break;
    case kToggleFollow:
        timeline_.setFollowPlayhead(!timeline_.followsPlayhead());
        followButton_.setToggleState(timeline_.followsPlayhead(), juce::dontSendNotification);
        commands_.commandStatusChanged();
        break;
    case kAudioSettings:
    {
        auto* selector =
            new juce::AudioDeviceSelectorComponent(deviceManager_, 0, 0, 2, 32, true, false, true, false);
        selector->setSize(560, 560);
        juce::DialogWindow::LaunchOptions options;
        options.content.setOwned(selector);
        options.dialogTitle = "Audio / MIDI settings";
        options.useNativeTitleBar = true;
        options.resizable = true;
        options.launchAsync();
        break;
    }
    default:
        return false;
    }
    if (document_.revision() != seenRevision_)
        documentChanged();
    return true;
}

// --- Menus ------------------------------------------------------------------

juce::StringArray MainComponent::getMenuBarNames()
{
    return {"File", "Edit", "View", "Audio"};
}

juce::PopupMenu MainComponent::getMenuForIndex(int index, const juce::String&)
{
    juce::PopupMenu menu;
    auto add = [&](std::initializer_list<juce::CommandID> ids) {
        for (auto id : ids)
            menu.addCommandItem(&commands_, id);
    };

    switch (index)
    {
    case 0:
        add({kNew, kOpen});
        menu.addSeparator();
        add({kSave, kSaveAs});
        break;
    case 1:
        add({kUndo, kRedo});
        menu.addSeparator();
        add({kDelete});
        menu.addSeparator();
        add({kNudgeEarlier, kNudgeLater, kNudgeEarlierFine, kNudgeLaterFine});
        menu.addSeparator();
        add({kPitchUp, kPitchDown});
        break;
    case 2:
        add({kPlayPause, kGoToStart});
        menu.addSeparator();
        add({kZoomIn, kZoomOut, kZoomSet, kZoomClip, kZoomTransition});
        menu.addSeparator();
        add({kToggleFollow});
        break;
    case 3:
    {
        add({kAudioSettings});
        menu.addSeparator();
        const bool direct = engine_.outputMode() == OutputMode::DirectOut;
        menu.addItem(kMenuModeInternal, "Internal Mix (laptop)", true, !direct);
        menu.addItem(kMenuModeDirect, "Direct Out (hardware mixer)", true, direct);
        menu.addSeparator();

        juce::PopupMenu decks;
        for (int n = 2; n <= kMaxDecks; ++n)
            decks.addItem(kMenuDecksBase + n, juce::String(n) + " decks", true, document_.set().numDecks == n);
        menu.addSubMenu("Decks", decks);

        // Pairs switched off in the device settings are shown but greyed out.
        const auto pairs = outputPairs();
        auto outputMenu = [&](int base, int noneOffset, int current) {
            juce::PopupMenu sub;
            sub.addItem(base + noneOffset, "None", true, current == Engine::kNoOutput);
            for (const auto& pair : pairs)
                sub.addItem(base + pair.firstChannel, "Outputs " + pair.name, pair.enabled,
                            current == pair.firstChannel);
            return sub;
        };
        menu.addSubMenu("Master Output", outputMenu(kMenuMasterBase, 999, engine_.masterOutput()), !direct);
        for (int d = 0; d < document_.set().numDecks; ++d)
            menu.addSubMenu("Deck " + juce::String(d + 1) + " Output",
                            outputMenu(kMenuDeckBase + d * 100, kMenuNone, engine_.deckOutput(d)), direct);
        break;
    }
    default:
        break;
    }
    return menu;
}

void MainComponent::menuItemSelected(int itemId, int)
{
    if (itemId == kMenuModeInternal || itemId == kMenuModeDirect)
    {
        setOutputMode(itemId == kMenuModeDirect ? OutputMode::DirectOut : OutputMode::InternalMix);
    }
    else if (itemId >= kMenuMasterBase && itemId < kMenuDeckBase)
    {
        const int ch = itemId - kMenuMasterBase;
        engine_.setMasterOutput(ch == 999 ? Engine::kNoOutput : ch);
        saveRouting();
        menuItemsChanged();
    }
    else if (itemId >= kMenuDeckBase && itemId < kMenuDecksBase)
    {
        const int deck = (itemId - kMenuDeckBase) / 100, ch = (itemId - kMenuDeckBase) % 100;
        engine_.setDeckOutput(deck, ch == kMenuNone ? Engine::kNoOutput : ch);
        saveRouting();
        menuItemsChanged();
    }
    else if (itemId > kMenuDecksBase && itemId <= kMenuDecksBase + kMaxDecks)
    {
        const int n = itemId - kMenuDecksBase;
        if (!document_.edit([n](Set& s) { return edit::setNumDecks(s, n); }) && n < document_.set().numDecks)
            showError("Can't remove decks", "Move or delete the clips on the decks you want to remove first.");
        documentChanged();
        menuItemsChanged();
    }
}

// --- Layout -----------------------------------------------------------------

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(kBackground);
}

void MainComponent::resized()
{
    auto area = getLocalBounds();

    auto bar = area.removeFromTop(42).reduced(8, 7);
    playButton_.setBounds(bar.removeFromLeft(70));
    bar.removeFromLeft(8);
    timeLabel_.setBounds(bar.removeFromLeft(100));
    bar.removeFromLeft(12);
    for (auto [button, width] : {std::pair<juce::Button*, int>{&zoomOutButton_, 30}, {&zoomInButton_, 30},
                                 {&zoomSetButton_, 50}, {&zoomClipButton_, 50}, {&zoomTransitionButton_, 80},
                                 {&followButton_, 70}})
    {
        button->setBounds(bar.removeFromLeft(width));
        bar.removeFromLeft(4);
    }
    settingsButton_.setBounds(bar.removeFromRight(70));
    bar.removeFromRight(8);
    decksBox_.setBounds(bar.removeFromRight(90));
    bar.removeFromRight(8);
    modeBox_.setBounds(bar.removeFromRight(210));
    bar.removeFromRight(8);
    deviceLabel_.setBounds(bar);

    library_.setBounds(area.removeFromLeft(240));
    inspector_.setBounds(area.removeFromRight(240));
    bottomTabs_.setBounds(area.removeFromBottom(150));
    timeline_.setBounds(area);
}

} // namespace vmp

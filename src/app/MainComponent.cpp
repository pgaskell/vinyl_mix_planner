#include "app/MainComponent.h"

#include "app/Commands.h"
#include "app/TimeFormat.h"
#include "core/Edits.h"
#include "core/SetFile.h"

namespace vmp
{

namespace
{
// Menu item ids outside the command range.
constexpr int kMenuModeInternal = 1001;
constexpr int kMenuModeDirect = 1002;
constexpr int kMenuMasterBase = 2000; // + first channel; kMenuMasterBase + 999 is "None"
constexpr int kMenuDeckBase = 3000;   // + deck * 100 + first channel; + 99 is "None"
constexpr int kMenuDecksBase = 4000;  // + number of decks
constexpr int kMenuNone = 99;

constexpr double kNudgeSeconds = 0.010;
constexpr double kFineNudgeSeconds = 0.001;
constexpr int kCoarsePitchSteps = 10; // 0.5%
constexpr double kGridShiftSeconds = 0.002;
constexpr double kGridBpmStep = 0.01;
constexpr int kMidiLogLines = 300;
const juce::Colour kBackground{0xff1b1d21};
} // namespace

MainComponent::MainComponent(juce::PropertiesFile& settings)
    : settings_(settings),
      analysisCache_(settings.getFile().getSiblingFile("analysis.json")),
      pool_(analysisCache_),
      timeline_(document_, pool_, engine_, display_),
      inspector_(document_, pool_, display_, commands_),
      mixer_(document_, pool_, display_, [this] { return engine_.playheadSeconds(); }),
      library_(pool_, settings)
{
    display_.keyNotation = static_cast<KeyNotation>(std::clamp(settings_.getIntValue("keyNotation", 2), 0, 2));

    // --- Commands, menus and shortcuts (user changes to the shortcuts are kept in settings) ---
    commands_.registerAllCommandsForTarget(this);
    commands_.setFirstCommandTarget(this);
    if (const auto mappings = settings_.getXmlValue("keyMappings"))
        commands_.getKeyMappings()->restoreFromXml(*mappings);
    commands_.getKeyMappings()->addChangeListener(this);
    inspector_.refreshShortcuts();
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
    addAndMakeVisible(mixer_);
    addAndMakeVisible(library_);
    library_.onFileDoubleClicked = [this](const juce::File& file) {
        addFileAtPlayhead(file);
        timeline_.grabKeyboardFocus();
    };

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
    commandButton(previousMixButton_, kPreviousMix, "Back to the previous mix (where two decks overlap)");
    commandButton(rewindButton_, kJumpBack, "Back by the number of beats chosen on the right");
    commandButton(playButton_, kPlayPause, "Play / pause (space)");
    commandButton(stopButton_, kStop, "Stop and return to where playback started (shift space)");
    commandButton(forwardButton_, kJumpForward, "Forward by the number of beats chosen on the right");
    commandButton(nextMixButton_, kNextMix, "On to the next mix (where two decks overlap)");
    for (int beats : {4, 8, 16, 32})
        beatJumpBox_.addItem(juce::String(beats) + " beats", beats);
    beatJumpBox_.setSelectedId(settings_.getIntValue("beatJump", 16), juce::dontSendNotification);
    if (beatJumpBox_.getSelectedId() == 0)
        beatJumpBox_.setSelectedId(16, juce::dontSendNotification);
    beatJumpBox_.setWantsKeyboardFocus(false);
    beatJumpBox_.setTooltip("How far << and >> jump");
    beatJumpBox_.onChange = [this] { settings_.setValue("beatJump", beatJumpBox_.getSelectedId()); };
    addAndMakeVisible(beatJumpBox_);
    commandButton(zoomOutButton_, kZoomOut, "Zoom out (cmd -)");
    commandButton(zoomInButton_, kZoomIn, "Zoom in (cmd =)");
    commandButton(zoomSetButton_, kZoomSet, "Zoom to the whole mix (shift Z)");
    commandButton(zoomClipButton_, kZoomClip, "Zoom to the selected clip (Z)");
    commandButton(zoomTransitionButton_, kZoomTransition, "Zoom to the mix into the selected clip (T)");
    commandButton(zoomBeatsButton_, kZoomBeats, "Zoom in on the beats at the playhead (B)");
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
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer(&timeline_)] {
        if (safe != nullptr)
            safe->grabKeyboardFocus();
    });
}

MainComponent::~MainComponent()
{
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(nullptr);
#endif
    saveRouting();
    commands_.getKeyMappings()->removeChangeListener(this);
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
        inspector_.refresh();
        rebuildSchedule();
        return;
    }
    if (source == commands_.getKeyMappings())
    {
        if (const auto xml = commands_.getKeyMappings()->createXml(true))
            settings_.setValue("keyMappings", xml.get());
        inspector_.refreshShortcuts();
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
    mixer_.update(timeline_.selectedClip(), engine_.playheadSeconds());
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
    if (decksBox_.getSelectedId() != set.numDecks)
    {
        decksBox_.setSelectedId(set.numDecks, juce::dontSendNotification);
        resized(); // the mixer panel's width depends on the deck count
    }
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

MainComponent::Selection MainComponent::selection() const
{
    Selection sel;
    sel.clip = document_.set().findClip(timeline_.selectedClip());
    sel.track = sel.clip != nullptr ? document_.set().findTrack(sel.clip->trackId) : nullptr;
    sel.timing = sel.track != nullptr ? timeline_.timingOf(sel.clip->id) : nullptr;
    return sel;
}

void MainComponent::select(Id clip)
{
    if (clip == 0)
        return;
    timeline_.setSelectedClip(clip);
    inspector_.setClip(clip);
}

void MainComponent::nudgeSelected(double seconds)
{
    const auto sel = selection();
    if (!sel)
        return;
    const Id id = sel.clip->id;
    const double start = sel.clip->startSeconds + seconds;
    const int deck = sel.clip->deck;
    document_.edit([&](Set& s) { return edit::moveClip(s, id, start, deck); });
}

void MainComponent::nudgeSelectedByBeats(int beats)
{
    const auto sel = selection();
    const auto grid = sel ? pool_.gridFor(*sel.track) : std::nullopt;
    if (!grid)
        return;
    // A beat as heard at the playhead (or the clip start if the playhead is elsewhere).
    const double at = std::clamp(engine_.playheadSeconds(), sel.timing->startSeconds(), sel.timing->endSeconds());
    nudgeSelected(beats * grid->periodSeconds() / sel.timing->speedAt(at));
}

void MainComponent::changeSelectedPitch(int steps, bool absolute)
{
    const auto sel = selection();
    if (!sel)
        return;
    const Id id = sel.clip->id;
    const int pitch = absolute ? steps : sel.clip->pitchSteps + steps;
    const double pivot = engine_.playheadSeconds();
    document_.edit([&](Set& s) { return edit::setClipPitch(s, id, pitch, pivot); });
}

void MainComponent::editSelectedGrid(
    const std::function<std::optional<BeatGrid>(const BeatGrid&, std::optional<double>)>& change)
{
    const auto sel = selection();
    if (!sel)
        return;
    const auto grid = pool_.gridFor(*sel.track);
    if (!grid)
    {
        showError("No beat grid", "No steady beat was found in this track. Type its BPM in the inspector first.");
        return;
    }
    const double playhead = engine_.playheadSeconds();
    std::optional<double> sourceAtPlayhead;
    if (playhead >= sel.timing->startSeconds() && playhead < sel.timing->endSeconds())
        sourceAtPlayhead = sel.timing->sourceFrameAt(playhead) / sel.track->sampleRate;

    const auto updated = change(*grid, sourceAtPlayhead);
    const Id trackId = sel.track->id;
    document_.edit([&](Set& s) { return edit::setTrackGrid(s, trackId, updated); });
}

namespace
{
// A new tempo for the grid that keeps the beat nearest `pivot` (source seconds) where it is.
BeatGrid retempo(const BeatGrid& grid, double bpm, std::optional<double> pivot)
{
    const double period = grid.periodSeconds();
    const double anchor = pivot ? *pivot : grid.firstBeatSeconds;
    const double k = std::round((anchor - grid.firstBeatSeconds) / period);
    const double beat = grid.firstBeatSeconds + k * period;
    return {bpm, beat - k * 60.0 / bpm};
}
} // namespace

double MainComponent::beatSecondsAtPlayhead() const
{
    const double playhead = engine_.playheadSeconds();
    const Set& set = document_.set();
    std::optional<double> best;
    for (const auto& clip : set.clips)
    {
        const Track* track = set.findTrack(clip.trackId);
        const auto grid = track != nullptr ? pool_.gridFor(*track) : std::nullopt;
        const ClipTiming* timing = timeline_.timingOf(clip.id);
        if (!grid || timing == nullptr || playhead < timing->startSeconds() || playhead >= timing->endSeconds())
            continue;
        const double beat = grid->periodSeconds() / timing->speedAt(playhead);
        if (clip.id == timeline_.selectedClip())
            return beat;
        if (!best)
            best = beat;
    }
    return best.value_or(0.5);
}

void MainComponent::jumpBeats(int direction)
{
    const double distance = beatJumpBox_.getSelectedId() * beatSecondsAtPlayhead();
    engine_.seek(std::max(0.0, engine_.playheadSeconds() + direction * distance));
}

void MainComponent::jumpToMix(int direction)
{
    // A little before the mix starts, so you hear it come in.
    constexpr double kLeadIn = 2.0;
    const double playhead = engine_.playheadSeconds();
    const auto mixes = timeline_.mixes();
    if (direction > 0)
    {
        for (const auto& [from, to] : mixes)
            if (from - kLeadIn > playhead + 0.05)
            {
                engine_.seek(std::max(0.0, from - kLeadIn));
                return;
            }
    }
    else
    {
        // Pressed just after a jump, go to the one before rather than the same one again.
        for (auto it = mixes.rbegin(); it != mixes.rend(); ++it)
            if (it->first - kLeadIn < playhead - 1.0)
            {
                engine_.seek(std::max(0.0, it->first - kLeadIn));
                return;
            }
        engine_.seek(0.0);
    }
}

void MainComponent::showKeyboardShortcuts()
{
    auto* editor = new juce::KeyMappingEditorComponent(*commands_.getKeyMappings(), true);
    editor->setSize(520, 640);
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(editor);
    options.dialogTitle = "Keyboard shortcuts";
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.launchAsync();
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
    for (juce::CommandID id = kNew; id <= kLastCommand; ++id)
        commands.add(id);
}

void MainComponent::getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info)
{
    using juce::KeyPress;
    const auto cmd = juce::ModifierKeys::commandModifier;
    const auto shift = juce::ModifierKeys::shiftModifier;
    const auto alt = juce::ModifierKeys::altModifier;
    const bool hasClip = static_cast<bool>(selection());

    auto define = [&](const char* name, const char* description, const char* category, bool active = true) {
        info.setInfo(name, description, category, 0);
        info.setActive(active);
    };
    auto key = [&](int code, juce::ModifierKeys mods = {}) { info.addDefaultKeypress(code, mods); };

    switch (id)
    {
    case kNew:
        define("New Set", "Start a new set", "File");
        key('n', cmd);
        break;
    case kOpen:
        define("Open...", "Open a set", "File");
        key('o', cmd);
        break;
    case kSave:
        define("Save", "Save the set", "File");
        key('s', cmd);
        break;
    case kSaveAs:
        define("Save As...", "Save the set to a new file", "File");
        key('s', cmd | shift);
        break;
    case kUndo:
        define("Undo", "Undo the last change", "Edit", document_.canUndo());
        key('z', cmd);
        break;
    case kRedo:
        define("Redo", "Redo the last undone change", "Edit", document_.canRedo());
        key('z', cmd | shift);
        break;
    case kDelete:
        define("Delete Clip", "Remove the selected clip", "Edit", hasClip);
        key(KeyPress::backspaceKey);
        key(KeyPress::deleteKey);
        break;
    case kKeyboardShortcuts:
        define("Keyboard Shortcuts...", "Change the keyboard shortcuts", "Edit");
        key('k', cmd);
        break;

    case kSelectDeck1:
    case kSelectDeck2:
    case kSelectDeck3:
    case kSelectDeck4:
    {
        const int deck = static_cast<int>(id - kSelectDeck1);
        static const char* const names[] = {"Select Deck 1", "Select Deck 2", "Select Deck 3", "Select Deck 4"};
        define(names[deck], "Select the record on this deck at the playhead", "Clip",
               deck < document_.set().numDecks);
        key('1' + deck);
        break;
    }
    case kSelectNextClip:
        define("Select Next Clip", "Select the next clip in time", "Clip");
        key(KeyPress::tabKey);
        break;
    case kSelectPreviousClip:
        define("Select Previous Clip", "Select the previous clip in time", "Clip");
        key(KeyPress::tabKey, shift);
        break;
    case kNudgeEarlier:
        define("Nudge Earlier", "Slip the selected record 10 ms earlier", "Clip", hasClip);
        key(KeyPress::leftKey);
        break;
    case kNudgeLater:
        define("Nudge Later", "Slip the selected record 10 ms later", "Clip", hasClip);
        key(KeyPress::rightKey);
        break;
    case kNudgeEarlierFine:
        define("Nudge Earlier (Fine)", "Slip the selected record 1 ms earlier", "Clip", hasClip);
        key(KeyPress::leftKey, shift);
        break;
    case kNudgeLaterFine:
        define("Nudge Later (Fine)", "Slip the selected record 1 ms later", "Clip", hasClip);
        key(KeyPress::rightKey, shift);
        break;
    case kNudgeBeatEarlier:
        define("Nudge One Beat Earlier", "Slip the selected record one beat earlier", "Clip", hasClip);
        key(KeyPress::leftKey, alt);
        break;
    case kNudgeBeatLater:
        define("Nudge One Beat Later", "Slip the selected record one beat later", "Clip", hasClip);
        key(KeyPress::rightKey, alt);
        break;
    case kPitchUp:
        define("Pitch +0.05%", "Speed up the selected record by one step", "Clip", hasClip);
        key(KeyPress::upKey);
        break;
    case kPitchDown:
        define("Pitch -0.05%", "Slow down the selected record by one step", "Clip", hasClip);
        key(KeyPress::downKey);
        break;
    case kPitchUpCoarse:
        define("Pitch +0.5%", "Speed up the selected record by ten steps", "Clip", hasClip);
        key(KeyPress::upKey, shift);
        break;
    case kPitchDownCoarse:
        define("Pitch -0.5%", "Slow down the selected record by ten steps", "Clip", hasClip);
        key(KeyPress::downKey, shift);
        break;
    case kPitchReset:
        define("Pitch to 0", "Pitch fader to the centre (quartz lock)", "Clip", hasClip);
        key('0');
        break;
    case kTrimStartToPlayhead:
        define("Trim Start to Playhead", "Start the selected clip at the playhead", "Clip", hasClip);
        key('i');
        break;
    case kTrimEndToPlayhead:
        define("Trim End to Playhead", "End the selected clip at the playhead", "Clip", hasClip);
        key('o');
        break;

    case kGridDownbeatHere:
        define("Downbeat at Playhead", "Put a bar line of the selected track's grid at the playhead", "Grid", hasClip);
        key('g');
        break;
    case kGridEarlier:
        define("Grid Earlier", "Move the selected track's beat grid 2 ms earlier", "Grid", hasClip);
        key('[');
        break;
    case kGridLater:
        define("Grid Later", "Move the selected track's beat grid 2 ms later", "Grid", hasClip);
        key(']');
        break;
    case kGridHalve:
        define("Halve BPM", "Halve the selected track's BPM", "Grid", hasClip);
        break;
    case kGridDouble:
        define("Double BPM", "Double the selected track's BPM", "Grid", hasClip);
        break;
    case kGridSlower:
        define("Grid BPM -0.01", "Slow the selected track's grid by 0.01 BPM", "Grid", hasClip);
        key('[', shift);
        break;
    case kGridFaster:
        define("Grid BPM +0.01", "Speed up the selected track's grid by 0.01 BPM", "Grid", hasClip);
        key(']', shift);
        break;
    case kGridReset:
        define("Reset Grid", "Go back to the detected tempo and grid", "Grid", hasClip);
        break;

    case kPlayPause:
        define("Play / Pause", "Start or stop playback", "Transport");
        key(KeyPress::spaceKey);
        break;
    case kGoToStart:
        define("Go to Start", "Move the playhead to the start", "Transport");
        key(KeyPress::homeKey);
        key(KeyPress::returnKey);
        break;
    case kJumpBack:
        define("Rewind", "Jump back by the chosen number of beats", "Transport");
        key(',');
        break;
    case kJumpForward:
        define("Fast Forward", "Jump forward by the chosen number of beats", "Transport");
        key('.');
        break;
    case kStop:
        define("Stop", "Stop and return to where playback started", "Transport");
        key(KeyPress::spaceKey, shift);
        break;
    case kPreviousMix:
        define("Previous Mix", "Jump to just before the previous mix", "Transport");
        key(',', shift);
        break;
    case kNextMix:
        define("Next Mix", "Jump to just before the next mix", "Transport");
        key('.', shift);
        break;
    case kCycleAutomationView:
        define("Show Next Automation", "Cycle which automation the selected clip's deck shows", "View", hasClip);
        key('a');
        break;

    case kZoomIn:
        define("Zoom In", "Zoom in around the playhead", "View");
        key('=', cmd);
        break;
    case kZoomOut:
        define("Zoom Out", "Zoom out", "View");
        key('-', cmd);
        break;
    case kZoomSet:
        define("Zoom to Whole Mix", "Fit the whole set in view", "View");
        key('z', shift);
        break;
    case kZoomClip:
        define("Zoom to Clip", "Fit the selected clip in view", "View");
        key('z');
        break;
    case kZoomTransition:
        define("Zoom to Transition", "Fit the mix into the selected clip in view", "View", hasClip);
        key('t');
        break;
    case kZoomBeats:
        define("Zoom to Beats", "Zoom in on the beats at the playhead", "View");
        key('b');
        break;
    case kToggleFollow:
        define("Follow Playhead", "Keep the playhead in view while playing", "View");
        info.setTicked(timeline_.followsPlayhead());
        key('f');
        break;
    case kToggleKeyNotation:
        define("Key Notation: Standard / Camelot / Both", "Change how keys are written", "View");
        key('k');
        break;

    case kAudioSettings:
        define("Audio / MIDI Settings...", "Choose audio and MIDI devices", "Audio");
        key(',', cmd);
        break;
    default:
        break;
    }
}

bool MainComponent::perform(const InvocationInfo& info)
{
    const double playhead = engine_.playheadSeconds();
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
    case kKeyboardShortcuts:
        showKeyboardShortcuts();
        break;

    case kSelectDeck1:
    case kSelectDeck2:
    case kSelectDeck3:
    case kSelectDeck4:
        select(timeline_.clipOnDeckNear(static_cast<int>(info.commandID - kSelectDeck1), playhead));
        break;
    case kSelectNextClip:
    case kSelectPreviousClip:
    {
        const auto order = timeline_.clipsInOrder();
        if (order.empty())
            break;
        auto it = std::find(order.begin(), order.end(), timeline_.selectedClip());
        const auto n = static_cast<long>(order.size());
        long index = it == order.end() ? -1 : it - order.begin();
        index = info.commandID == kSelectNextClip ? (index + 1) % n : (index <= 0 ? n - 1 : index - 1);
        select(order[static_cast<size_t>(index)]);
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
    case kNudgeBeatEarlier:
        nudgeSelectedByBeats(-1);
        break;
    case kNudgeBeatLater:
        nudgeSelectedByBeats(1);
        break;
    case kPitchUp:
        changeSelectedPitch(1);
        break;
    case kPitchDown:
        changeSelectedPitch(-1);
        break;
    case kPitchUpCoarse:
        changeSelectedPitch(kCoarsePitchSteps);
        break;
    case kPitchDownCoarse:
        changeSelectedPitch(-kCoarsePitchSteps);
        break;
    case kPitchReset:
        changeSelectedPitch(0, true);
        break;
    case kTrimStartToPlayhead:
    case kTrimEndToPlayhead:
    {
        const Id id = timeline_.selectedClip();
        const bool start = info.commandID == kTrimStartToPlayhead;
        document_.edit([&](Set& s) {
            return start ? edit::trimClipStart(s, id, playhead) : edit::trimClipEnd(s, id, playhead);
        });
        break;
    }

    case kGridDownbeatHere:
        editSelectedGrid([](const BeatGrid& grid, std::optional<double> here) -> std::optional<BeatGrid> {
            return here ? BeatGrid{grid.bpm, *here} : grid;
        });
        break;
    case kGridEarlier:
    case kGridLater:
    {
        const double shift = info.commandID == kGridEarlier ? -kGridShiftSeconds : kGridShiftSeconds;
        editSelectedGrid([shift](const BeatGrid& grid, std::optional<double>) -> std::optional<BeatGrid> {
            return BeatGrid{grid.bpm, grid.firstBeatSeconds + shift};
        });
        break;
    }
    case kGridHalve:
    case kGridDouble:
    case kGridSlower:
    case kGridFaster:
    {
        const auto command = info.commandID;
        editSelectedGrid([command](const BeatGrid& grid, std::optional<double> here) -> std::optional<BeatGrid> {
            const double bpm = command == kGridHalve    ? grid.bpm / 2.0
                               : command == kGridDouble ? grid.bpm * 2.0
                               : command == kGridSlower ? grid.bpm - kGridBpmStep
                                                        : grid.bpm + kGridBpmStep;
            return retempo(grid, bpm, here);
        });
        break;
    }
    case kGridReset:
    {
        const auto sel = selection();
        if (!sel)
            break;
        const Id trackId = sel.track->id;
        document_.edit([&](Set& s) { return edit::setTrackGrid(s, trackId, std::nullopt); });
        break;
    }

    case kPlayPause:
        if (engine_.isPlaying())
            engine_.pause();
        else
        {
            playStartedAt_ = playhead;
            engine_.play();
        }
        break;
    case kStop:
        engine_.pause();
        engine_.seek(playStartedAt_);
        break;
    case kPreviousMix:
        jumpToMix(-1);
        break;
    case kNextMix:
        jumpToMix(1);
        break;
    case kCycleAutomationView:
        if (const auto sel = selection())
            timeline_.cycleAutomationView(sel.clip->deck);
        break;
    case kGoToStart:
        engine_.seek(0.0);
        break;
    case kJumpBack:
        jumpBeats(-1);
        break;
    case kJumpForward:
        jumpBeats(1);
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
    case kZoomBeats:
        timeline_.zoomToBeats();
        break;
    case kToggleFollow:
        timeline_.setFollowPlayhead(!timeline_.followsPlayhead());
        followButton_.setToggleState(timeline_.followsPlayhead(), juce::dontSendNotification);
        commands_.commandStatusChanged();
        break;
    case kToggleKeyNotation:
        display_.keyNotation = static_cast<KeyNotation>((static_cast<int>(display_.keyNotation) + 1) % 3);
        settings_.setValue("keyNotation", static_cast<int>(display_.keyNotation));
        timeline_.repaint();
        inspector_.refresh();
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
    return {"File", "Edit", "Clip", "Grid", "View", "Audio"};
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
        add({kKeyboardShortcuts});
        break;
    case 2:
        add({kSelectDeck1, kSelectDeck2, kSelectDeck3, kSelectDeck4, kSelectNextClip, kSelectPreviousClip});
        menu.addSeparator();
        add({kPitchUp, kPitchDown, kPitchUpCoarse, kPitchDownCoarse, kPitchReset});
        menu.addSeparator();
        add({kNudgeEarlier, kNudgeLater, kNudgeEarlierFine, kNudgeLaterFine, kNudgeBeatEarlier, kNudgeBeatLater});
        menu.addSeparator();
        add({kTrimStartToPlayhead, kTrimEndToPlayhead});
        break;
    case 3:
        add({kGridDownbeatHere, kGridEarlier, kGridLater, kGridSlower, kGridFaster, kGridHalve, kGridDouble,
             kGridReset});
        break;
    case 4:
        add({kPlayPause, kStop, kGoToStart, kJumpBack, kJumpForward, kPreviousMix, kNextMix});
        menu.addSeparator();
        add({kZoomIn, kZoomOut, kZoomSet, kZoomClip, kZoomTransition, kZoomBeats});
        menu.addSeparator();
        add({kToggleFollow, kToggleKeyNotation, kCycleAutomationView});
        break;
    case 5:
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
    for (auto [button, width] : {std::pair<juce::Component*, int>{&previousMixButton_, 34}, {&rewindButton_, 34},
                                 {&playButton_, 56}, {&stopButton_, 48}, {&forwardButton_, 34}, {&nextMixButton_, 34},
                                 {&beatJumpBox_, 92}})
    {
        button->setBounds(bar.removeFromLeft(width));
        bar.removeFromLeft(3);
    }
    bar.removeFromLeft(6);
    timeLabel_.setBounds(bar.removeFromLeft(100));
    bar.removeFromLeft(10);
    for (auto [button, width] : {std::pair<juce::Button*, int>{&zoomOutButton_, 28}, {&zoomInButton_, 28},
                                 {&zoomSetButton_, 44}, {&zoomClipButton_, 44}, {&zoomTransitionButton_, 76},
                                 {&zoomBeatsButton_, 50}, {&followButton_, 66}})
    {
        button->setBounds(bar.removeFromLeft(width));
        bar.removeFromLeft(3);
    }
    settingsButton_.setBounds(bar.removeFromRight(64));
    bar.removeFromRight(6);
    decksBox_.setBounds(bar.removeFromRight(86));
    bar.removeFromRight(6);
    modeBox_.setBounds(bar.removeFromRight(196));
    bar.removeFromRight(6);
    deviceLabel_.setBounds(bar);

    library_.setBounds(area.removeFromLeft(220));
    auto right = area.removeFromRight(std::max(250, MixerPanel::preferredWidth(document_.set().numDecks)));
    inspector_.setBounds(right.removeFromTop(300));
    mixer_.setBounds(right);
    bottomTabs_.setBounds(area.removeFromBottom(130));
    timeline_.setBounds(area);
}

} // namespace vmp

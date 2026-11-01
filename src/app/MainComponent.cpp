#include "app/MainComponent.h"

namespace vmp
{

namespace
{
constexpr int kMidiLogLines = 300;
constexpr int kModeInternalId = 1;
constexpr int kModeDirectId = 2;
} // namespace

MainComponent::MainComponent(juce::PropertiesFile& settings) : settings_(settings)
{
    restoreSettings();

    addAndMakeVisible(settingsButton_);
    settingsButton_.onClick = [this] { showAudioSettings(); };
    addAndMakeVisible(deviceLabel_);

    addAndMakeVisible(modeLabel_);
    addAndMakeVisible(modeBox_);
    modeBox_.addItem("Internal mix (laptop)", kModeInternalId);
    modeBox_.addItem("Direct out (hardware mixer)", kModeDirectId);
    modeBox_.onChange = [this] {
        setOutputMode(modeBox_.getSelectedId() == kModeDirectId ? OutputMode::DirectOut : OutputMode::InternalMix);
    };

    addAndMakeVisible(deckCountLabel_);
    addAndMakeVisible(deckCountBox_);
    for (int n = 2; n <= Engine::kMaxDecks; ++n)
        deckCountBox_.addItem(juce::String(n), n);
    deckCountBox_.onChange = [this] { setNumDecks(deckCountBox_.getSelectedId()); };

    addAndMakeVisible(masterLabel_);
    addAndMakeVisible(masterBox_);
    masterBox_.onChange = [this] {
        const int id = masterBox_.getSelectedId();
        if (id != 0)
            engine_.setMasterOutput(id == kNoOutputId ? Engine::kNoOutput : id - 1);
    };

    for (int i = 0; i < Engine::kMaxDecks; ++i)
    {
        decks_[static_cast<size_t>(i)] = std::make_unique<DeckStrip>(i, engine_, loader_, thumbnailCache_);
        addChildComponent(*decks_[static_cast<size_t>(i)]);
    }

    addAndMakeVisible(midiTitle_);
    addAndMakeVisible(midiLog_);
    midiLog_.setMultiLine(true);
    midiLog_.setReadOnly(true);
    midiLog_.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));

    const auto savedDevice = settings_.getXmlValue("audioDeviceState");
    const auto error = deviceManager_.initialise(0, 2, savedDevice.get(), true);
    if (savedDevice == nullptr)
        for (const auto& input : juce::MidiInput::getAvailableDevices())
            deviceManager_.setMidiInputDeviceEnabled(input.identifier, true);

    deviceManager_.addAudioCallback(this);
    deviceManager_.addMidiInputDeviceCallback({}, this);
    deviceManager_.addChangeListener(this);

    setNumDecks(engine_.numDecks());
    setOutputMode(engine_.outputMode());
    refreshDeviceInfo();
    if (error.isNotEmpty())
        deviceLabel_.setText("Audio device error: " + error, juce::dontSendNotification);

    startTimerHz(30);
    setSize(1100, 760);
}

MainComponent::~MainComponent()
{
    saveRoutingSettings();
    deviceManager_.removeChangeListener(this);
    deviceManager_.removeMidiInputDeviceCallback({}, this);
    deviceManager_.removeAudioCallback(this);
    deviceManager_.closeAudioDevice();
}

// --- Audio ------------------------------------------------------------------

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

// --- MIDI -------------------------------------------------------------------

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

// --- Message thread ---------------------------------------------------------

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster*)
{
    if (auto state = deviceManager_.createStateXml())
        settings_.setValue("audioDeviceState", state.get());
    refreshDeviceInfo();
}

void MainComponent::timerCallback()
{
    for (int i = 0; i < engine_.numDecks(); ++i)
        decks_[static_cast<size_t>(i)]->update();
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

void MainComponent::showAudioSettings()
{
    auto* selector = new juce::AudioDeviceSelectorComponent(deviceManager_, 0, 0, 2, 32, true, false, true, false);
    selector->setSize(560, 560);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(selector);
    options.dialogTitle = "Audio / MIDI settings";
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.launchAsync();
}

void MainComponent::refreshDeviceInfo()
{
    std::vector<OutputChoice> choices;
    if (auto* device = deviceManager_.getCurrentAudioDevice())
    {
        const auto names = device->getOutputChannelNames();
        const auto active = device->getActiveOutputChannels();
        for (int ch = 0; ch + 1 < names.size(); ch += 2)
            choices.push_back({ch, juce::String(ch + 1) + "/" + juce::String(ch + 2), active[ch] && active[ch + 1]});

        deviceLabel_.setText(device->getName() + "   " + juce::String(device->getCurrentSampleRate() / 1000.0, 1) +
                                 " kHz, " + juce::String(device->getCurrentBufferSizeSamples()) + " samples, " +
                                 juce::String(active.countNumberOfSetBits()) + " of " +
                                 juce::String(names.size()) + " outputs on",
                             juce::dontSendNotification);
    }
    else
    {
        deviceLabel_.setText("No audio device", juce::dontSendNotification);
    }

    fillOutputBox(masterBox_, choices, engine_.masterOutput());
    for (auto& deck : decks_)
        deck->setOutputChoices(choices);
}

void MainComponent::setNumDecks(int count)
{
    engine_.setNumDecks(count);
    const int n = engine_.numDecks();
    for (int i = 0; i < Engine::kMaxDecks; ++i)
        decks_[static_cast<size_t>(i)]->setVisible(i < n);
    deckCountBox_.setSelectedId(n, juce::dontSendNotification);
    resized();
}

void MainComponent::setOutputMode(OutputMode mode)
{
    engine_.setOutputMode(mode);
    const bool direct = mode == OutputMode::DirectOut;
    modeBox_.setSelectedId(direct ? kModeDirectId : kModeInternalId, juce::dontSendNotification);
    masterBox_.setEnabled(!direct);
    for (auto& deck : decks_)
        deck->setDirectOut(direct);
}

void MainComponent::restoreSettings()
{
    engine_.setOutputMode(settings_.getIntValue("outputMode", 0) == 1 ? OutputMode::DirectOut
                                                                       : OutputMode::InternalMix);
    engine_.setNumDecks(settings_.getIntValue("numDecks", 2));
    engine_.setMasterOutput(settings_.getIntValue("masterOutput", 0));
    for (int i = 0; i < Engine::kMaxDecks; ++i)
        engine_.setDeckOutput(i, settings_.getIntValue("deckOutput" + juce::String(i), i * 2));
}

void MainComponent::saveRoutingSettings()
{
    settings_.setValue("outputMode", engine_.outputMode() == OutputMode::DirectOut ? 1 : 0);
    settings_.setValue("numDecks", engine_.numDecks());
    settings_.setValue("masterOutput", engine_.masterOutput());
    for (int i = 0; i < Engine::kMaxDecks; ++i)
        settings_.setValue("deckOutput" + juce::String(i), engine_.deckOutput(i));
}

// --- Layout -----------------------------------------------------------------

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1b1d21));
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced(8);

    auto top = area.removeFromTop(28);
    settingsButton_.setBounds(top.removeFromLeft(180));
    top.removeFromLeft(10);
    masterBox_.setBounds(top.removeFromRight(90));
    masterLabel_.setBounds(top.removeFromRight(80));
    top.removeFromRight(10);
    deckCountBox_.setBounds(top.removeFromRight(60));
    deckCountLabel_.setBounds(top.removeFromRight(48));
    top.removeFromRight(10);
    modeBox_.setBounds(top.removeFromRight(220));
    modeLabel_.setBounds(top.removeFromRight(48));
    deviceLabel_.setBounds(top);

    area.removeFromTop(8);
    auto midi = area.removeFromBottom(150);
    midiTitle_.setBounds(midi.removeFromTop(20));
    midiLog_.setBounds(midi);
    area.removeFromBottom(8);

    const int n = engine_.numDecks();
    const int height = area.getHeight() / n;
    for (int i = 0; i < n; ++i)
        decks_[static_cast<size_t>(i)]->setBounds(area.removeFromTop(height).reduced(0, 3));
}

} // namespace vmp

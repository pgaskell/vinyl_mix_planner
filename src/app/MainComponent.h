#pragma once

#include "app/AnalysisCache.h"
#include "app/AudioPool.h"
#include "app/Display.h"
#include "app/Inspector.h"
#include "app/IssuesPanel.h"
#include "app/LibraryPanel.h"
#include "app/MixerPanel.h"
#include "app/TimelineView.h"
#include "core/Document.h"
#include "engine/Engine.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

namespace vmp
{

class MainComponent : public juce::Component,
                      public juce::DragAndDropContainer,
                      public juce::ApplicationCommandTarget,
                      public juce::MenuBarModel,
                      private juce::AudioIODeviceCallback,
                      private juce::MidiInputCallback,
                      private juce::ChangeListener,
                      private juce::Timer
{
public:
    explicit MainComponent(juce::PropertiesFile& settings);
    ~MainComponent() override;

    // Offers to save unsaved changes, then runs `then` unless the user cancels.
    void confirmDiscard(std::function<void()> then);

    void paint(juce::Graphics& g) override;
    void resized() override;

    // ApplicationCommandTarget
    juce::ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
    void getAllCommands(juce::Array<juce::CommandID>& commands) override;
    void getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info) override;
    bool perform(const InvocationInfo& info) override;

    // MenuBarModel
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int index, const juce::String& name) override;
    void menuItemSelected(int itemId, int menuIndex) override;

private:
    // Audio thread.
    void audioDeviceIOCallbackWithContext(const float* const* inputs, int numInputs, float* const* outputs,
                                          int numOutputs, int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override {}

    // MIDI thread.
    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;

    // Message thread.
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void timerCallback() override;

    void documentChanged();
    void rebuildSchedule();
    void updateTitle();
    void updateDeviceInfo();

    std::optional<double> addFile(const juce::File& file, int deck, double start);
    void addFileAtPlayhead(const juce::File& file);

    // The selected clip with its track and timing; empty if nothing usable is selected.
    struct Selection
    {
        const Clip* clip = nullptr;
        const Track* track = nullptr;
        const ClipTiming* timing = nullptr;
        explicit operator bool() const { return timing != nullptr; }
    };
    Selection selection() const;
    void select(Id clip);
    void nudgeSelected(double seconds);
    void nudgeSelectedByBeats(int beats);
    void changeSelectedPitch(int steps, bool absolute = false);
    // Changes the selected track's beat grid; `change` gets the current grid and the source
    // time (seconds) under the playhead, or nullopt when the playhead is outside the clip.
    void editSelectedGrid(const std::function<std::optional<BeatGrid>(const BeatGrid&, std::optional<double>)>& change);
    void showKeyboardShortcuts();
    // One beat of the music at the playhead (the selected clip if it's playing, else any that
    // is), for beat jumps. Falls back to 120 BPM.
    double beatSecondsAtPlayhead() const;
    void jumpBeats(int direction);
    void jumpToMix(int direction);

    void newSet();
    void openSet();
    void openSetFile(const juce::File& file);
    void save(bool chooseFile, std::function<void()> onSaved = {});
    bool writeSet(const juce::File& file);
    void showError(const juce::String& title, const juce::String& message);

    void setOutputMode(OutputMode mode);
    void restoreRouting();
    void saveRouting();
    struct OutputPair
    {
        int firstChannel;
        juce::String name;
        bool enabled;
    };
    std::vector<OutputPair> outputPairs();

    juce::PropertiesFile& settings_;
    SetDocument document_;
    juce::File file_;
    uint64_t seenRevision_ = ~uint64_t{0};

    Engine engine_;
    AnalysisCache analysisCache_;
    AudioPool pool_;
    juce::AudioDeviceManager deviceManager_;
    juce::ApplicationCommandManager commands_;
    DisplayOptions display_;

    TimelineView timeline_;
    Inspector inspector_;
    MixerPanel mixer_;
    LibraryPanel library_;
    IssuesPanel issues_;
    juce::TextEditor midiLog_;
    juce::TabbedComponent bottomTabs_{juce::TabbedButtonBar::TabsAtTop};

    juce::TextButton previousMixButton_{"|<"}, rewindButton_{"<<"}, playButton_{"Play"}, stopButton_{"Stop"},
        forwardButton_{">>"}, nextMixButton_{">|"};
    juce::ComboBox beatJumpBox_;
    double playStartedAt_ = 0.0; // where Stop returns to
    juce::Label timeLabel_;
    juce::TextButton zoomOutButton_{"-"}, zoomInButton_{"+"}, zoomSetButton_{"Mix"}, zoomClipButton_{"Clip"},
        zoomTransitionButton_{"Transition"}, zoomBeatsButton_{"Beats"};
    juce::ToggleButton followButton_{"Follow"};
    juce::ComboBox modeBox_, decksBox_;
    juce::TextButton settingsButton_{"Audio..."};
    juce::Label deviceLabel_;

    juce::ScopedMessageBox messageBox_;
    std::unique_ptr<juce::FileChooser> chooser_;

    juce::CriticalSection midiLock_;
    juce::StringArray pendingMidi_; // guarded by midiLock_
    juce::StringArray midiLines_;
};

} // namespace vmp

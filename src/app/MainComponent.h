#pragma once

#include "app/DeckStrip.h"
#include "app/TrackLoader.h"
#include "engine/Engine.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <array>
#include <memory>

namespace vmp
{

// Phase 0 prototype window: decks, output mode and routing, and a MIDI monitor.
class MainComponent : public juce::Component,
                      private juce::AudioIODeviceCallback,
                      private juce::MidiInputCallback,
                      private juce::ChangeListener,
                      private juce::Timer
{
public:
    explicit MainComponent(juce::PropertiesFile& settings);
    ~MainComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

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
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void showAudioSettings();
    void refreshDeviceInfo();
    void setNumDecks(int count);
    void setOutputMode(OutputMode mode);
    void restoreSettings();
    void saveRoutingSettings();

    juce::PropertiesFile& settings_;
    Engine engine_;
    TrackLoader loader_;
    juce::AudioDeviceManager deviceManager_;
    juce::AudioThumbnailCache thumbnailCache_{8};

    juce::TextButton settingsButton_{"Audio / MIDI settings..."};
    juce::Label deviceLabel_;
    juce::Label modeLabel_{{}, "Mode"};
    juce::ComboBox modeBox_;
    juce::Label deckCountLabel_{{}, "Decks"};
    juce::ComboBox deckCountBox_;
    juce::Label masterLabel_{{}, "Master out"};
    juce::ComboBox masterBox_;
    std::array<std::unique_ptr<DeckStrip>, Engine::kMaxDecks> decks_;
    juce::Label midiTitle_{{}, "MIDI monitor"};
    juce::TextEditor midiLog_;

    juce::CriticalSection midiLock_;
    juce::StringArray pendingMidi_; // guarded by midiLock_
    juce::StringArray midiLines_;
};

} // namespace vmp

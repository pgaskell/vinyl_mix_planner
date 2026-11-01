#pragma once

#include "app/TrackLoader.h"
#include "engine/Engine.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <vector>

namespace vmp
{

// A stereo output pair offered in the routing menus.
struct OutputChoice
{
    int firstChannel; // physical, zero-based
    juce::String name;
    bool enabled;     // the channels are switched on in the device settings
};

// Fills a routing menu. Item ids are firstChannel + 1; kNoOutput maps to kNoOutputId.
void fillOutputBox(juce::ComboBox& box, const std::vector<OutputChoice>& choices, int selectedFirstChannel);
inline constexpr int kNoOutputId = 1000;

class WaveformView : public juce::Component, private juce::ChangeListener
{
public:
    WaveformView(juce::AudioFormatManager& formats, juce::AudioThumbnailCache& cache);
    ~WaveformView() override;

    void setFile(const juce::File& file);
    void setPlayhead(double seconds);
    std::function<void(double seconds)> onSeek;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override { repaint(); }
    void seekTo(float x);

    juce::AudioThumbnail thumbnail_;
    double playheadSeconds_ = 0.0;
};

// One turntable: load, start/stop, pitch fader, channel fader, output routing and waveform.
class DeckStrip : public juce::Component, public juce::FileDragAndDropTarget
{
public:
    DeckStrip(int index, Engine& engine, TrackLoader& loader, juce::AudioThumbnailCache& cache);

    void setOutputChoices(const std::vector<OutputChoice>& choices);
    void setDirectOut(bool directOut);
    void loadFile(const juce::File& file);

    // Called from the UI timer.
    void update();

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;
    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void trackLoaded(const juce::File& file, std::unique_ptr<TrackAudio> track, const juce::String& error);
    void updatePitchLabel();
    Deck& deck() { return engine_.deck(index_); }

    const int index_;
    Engine& engine_;
    TrackLoader& loader_;

    juce::Label title_;
    juce::TextButton loadButton_{"Load..."};
    juce::TextButton startStopButton_{"Start/Stop"};
    juce::Label pitchLabel_{{}, "Pitch"};
    juce::Slider pitchSlider_;
    juce::Label keyShiftLabel_;
    juce::Label volumeLabel_{{}, "Vol"};
    juce::Slider volumeSlider_;
    juce::ComboBox outputBox_;
    WaveformView waveform_;
    std::unique_ptr<juce::FileChooser> chooser_;

    double trackSampleRate_ = 0.0;
};

} // namespace vmp

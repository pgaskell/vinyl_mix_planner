#pragma once

#include "engine/Deck.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <functional>
#include <memory>

namespace vmp
{

// Decodes audio files to TrackAudio on a background thread.
class TrackLoader
{
public:
    // Runs on the message thread. track is null on failure, with error set.
    using Callback = std::function<void(std::unique_ptr<TrackAudio> track, juce::String error)>;

    TrackLoader();

    juce::AudioFormatManager& formats() { return formats_; }
    bool canLoad(const juce::File& file);

    // The callback may arrive after its caller is gone: capture a SafePointer, not `this`.
    void load(const juce::File& file, Callback onDone);

private:
    juce::AudioFormatManager formats_;
    juce::ThreadPool pool_{2}; // declared after formats_ so it joins before formats_ is destroyed
};

} // namespace vmp

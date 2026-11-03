#pragma once

#include "engine/Schedule.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace vmp
{

// Exports the set's internal mix to an audio file: asks for the format and the file, renders it
// on a background thread behind a progress window (engine/Bounce, the same code as playback),
// then says how it went, including whether anything clipped. The file appears only once it's
// complete; cancelling or a failure leaves nothing behind.
class MixExporter
{
public:
    explicit MixExporter(juce::PropertiesFile& settings);
    ~MixExporter();

    // `schedule` is the set as it is now; `name` suggests the file name.
    void start(std::unique_ptr<Schedule> schedule, const juce::String& name);
    bool isRunning() const { return job_ != nullptr || options_ != nullptr || chooser_ != nullptr; }

private:
    class Job;
    void chooseFile(int format, double sampleRate);
    void finished(const juce::String& title, const juce::String& message, bool warning);

    juce::PropertiesFile& settings_;
    std::unique_ptr<Schedule> schedule_;
    juce::String name_;
    std::unique_ptr<juce::AlertWindow> options_;
    std::unique_ptr<juce::FileChooser> chooser_;
    std::unique_ptr<Job> job_;
};

} // namespace vmp

#pragma once

#include <juce_core/juce_core.h>

namespace vmp
{

// The app's log: what it did (devices, sets opened and saved, exports, errors, glitches), kept to
// about half a megabyte. Written through juce::Logger, on the message thread only.
juce::File logFile();
std::unique_ptr<juce::FileLogger> openLog(const juce::String& welcome);

// What a problem report holds besides the log and macOS's crash reports for the app.
struct ProblemReport
{
    juce::String description; // in the tester's words
    juce::String info;        // app, system, audio device, settings
    juce::String glitches;
    juce::String setJson; // the set as it is now, unsaved changes included; no audio
};

// Writes the report as a zip. No audio, but the set names the tracks and their file paths.
bool writeProblemReport(const juce::File& zip, const ProblemReport& report);

} // namespace vmp

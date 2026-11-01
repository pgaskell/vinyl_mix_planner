#pragma once

#include <juce_core/juce_core.h>

#include <cmath>

namespace vmp
{

// "m:ss" or "m:ss.mmm"; negative times get a leading "-".
inline juce::String formatTime(double seconds, bool withMillis)
{
    const bool negative = seconds < 0.0;
    seconds = std::abs(seconds);
    const auto totalMillis = static_cast<int64_t>(std::llround(seconds * 1000.0));
    const auto minutes = totalMillis / 60000;
    const auto secs = (totalMillis / 1000) % 60;
    juce::String text = (negative ? "-" : "") + juce::String(minutes) + ":" + juce::String(secs).paddedLeft('0', 2);
    if (withMillis)
        text += "." + juce::String(totalMillis % 1000).paddedLeft('0', 3);
    return text;
}

} // namespace vmp

#pragma once

#include "core/Musical.h"

#include <juce_core/juce_core.h>

#include <cmath>

namespace vmp
{

enum class KeyNotation
{
    Standard,
    Camelot,
    Both
};

struct DisplayOptions
{
    KeyNotation keyNotation = KeyNotation::Both;
};

inline juce::String keyText(MusicalKey key, KeyNotation notation)
{
    switch (notation)
    {
    case KeyNotation::Standard:
        return key.name();
    case KeyNotation::Camelot:
        return key.camelot();
    case KeyNotation::Both:
        break;
    }
    return juce::String(key.camelot()) + " " + key.name();
}

// The key heard at this speed, with how far off it is when that's noticeable (>= 5 cents).
inline juce::String pitchedKeyText(MusicalKey key, double speed, KeyNotation notation)
{
    const auto pitched = pitchedKey(key, speed);
    juce::String text = keyText(pitched.key, notation);
    if (std::abs(pitched.cents) >= 5.0)
        text << juce::String::formatted(" %+.0fc", pitched.cents);
    return text;
}

} // namespace vmp

#pragma once

#include "core/Set.h"

#include <juce_graphics/juce_graphics.h>

#include <algorithm>
#include <array>

namespace vmp
{

// One colour per deck, used wherever a deck appears (lane, clips, channel strip, pitch) so it's
// recognisable everywhere. Kept clear of the hardware blue (0xff4fc3f7), the warning red and the
// automated orange.
inline juce::Colour deckColour(int deck)
{
    static const std::array<juce::Colour, kMaxDecks> colours{juce::Colour(0xff8a7cf0), juce::Colour(0xffd98f4f),
                                                             juce::Colour(0xff69c27a), juce::Colour(0xffc06fd0)};
    return colours[static_cast<size_t>(std::clamp(deck, 0, kMaxDecks - 1))];
}

} // namespace vmp

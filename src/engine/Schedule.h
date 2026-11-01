#pragma once

#include "core/Set.h"
#include "core/TrackAudio.h"

#include <array>
#include <functional>
#include <memory>
#include <vector>

namespace vmp
{

// A snapshot of the set prepared for playback: per deck, its clips in start order with their
// timing and audio. Built on the message thread after every edit and handed to the engine.
struct ScheduledClip
{
    Clip clip; // a copy: its mixer settings and automation
    double start, end;
    ClipTiming timing;
    std::shared_ptr<const TrackAudio> audio; // keeps the audio alive while the engine uses it; null if not loaded
};

struct Schedule
{
    int numDecks = 2;
    std::array<std::vector<ScheduledClip>, kMaxDecks> lanes;
};

// Returns the decoded audio for a track, or null if it isn't loaded (yet).
using AudioLookup = std::function<std::shared_ptr<const TrackAudio>(const Track&)>;

// Clips whose audio isn't loaded (yet) are kept with null audio: they play silence, and the
// engine reports it as a glitch.
std::unique_ptr<Schedule> buildSchedule(const Set& set, const AudioLookup& lookup);

} // namespace vmp

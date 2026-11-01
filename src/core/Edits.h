#pragma once

#include "core/Set.h"

#include <optional>

namespace vmp::edit
{

// Shortest clip a trim can leave.
inline constexpr double kMinClipSeconds = 0.5;

// Each edit returns false and leaves the set untouched if it can't be applied.
// Times are timeline seconds.

// Adds the track, or returns the existing one with the same path.
Id addTrack(Set& set, Track track);

// Places the whole track on a deck with its needle drop at startSeconds.
Id addClip(Set& set, Id trackId, int deck, double startSeconds);

// Removes the clip, and its track if no other clip uses it.
bool deleteClip(Set& set, Id clipId);

bool moveClip(Set& set, Id clipId, double startSeconds, int deck);

// Trims keep the music where it is on the timeline: trimming the start later moves the needle
// drop to the music that was playing at that time. The start can't be trimmed past the first
// pitch move, since moves are anchored to the music after the needle drop.
bool trimClipStart(Set& set, Id clipId, double newStartSeconds);
bool trimClipEnd(Set& set, Id clipId, double newEndSeconds);

// Changes the pitch the clip starts with. The music at `pivotSeconds` stays where it is, as when
// you move the pitch fader on a playing deck; without a pivot (or outside the clip) the needle
// drop stays put and everything after it stretches.
bool setClipPitch(Set& set, Id clipId, int pitchSteps, std::optional<double> pivotSeconds = std::nullopt);

bool setClipMixer(Set& set, Id clipId, const ChannelSettings& settings);

bool setNumDecks(Set& set, int numDecks);

// Corrections to the analysis; nullopt goes back to the detected values.
bool setTrackGrid(Set& set, Id trackId, std::optional<BeatGrid> grid);
bool setTrackKey(Set& set, Id trackId, std::optional<MusicalKey> key);

} // namespace vmp::edit

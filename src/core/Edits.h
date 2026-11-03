#pragma once

#include "core/Set.h"

#include <functional>
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
// The clip's trim knob (0..1, centre 0.5). One value for the whole clip.
bool setClipTrim(Set& set, Id clipId, double trim);

// --- Automation. Values are clamped to the control's range; pitch to whole steps. ---

// Within this distance, a control change at the playhead updates the existing keypoint.
inline constexpr double kKeypointSnapSeconds = 0.010;

// Adds a keypoint, or replaces the one at exactly that frame.
bool setAutomationPoint(Set& set, Id clipId, Param param, int64_t sourceFrame, double value);
// Moves keypoint `index`; it stays between its neighbours, so indexes don't change.
bool moveAutomationPoint(Set& set, Id clipId, Param param, size_t index, int64_t sourceFrame, double value);
bool deleteAutomationPoint(Set& set, Id clipId, Param param, size_t index);
// Removes all keypoints; the value at the start of the clip becomes its starting value.
bool clearAutomation(Set& set, Id clipId, Param param);

// Moves the control's starting point (at the needle drop). Pitch: the needle drop stays put.
bool setStartValue(Set& set, Id clipId, Param param, double value);

// What turning a control on the mixer does. With no keypoints it changes the clip's starting
// value; with keypoints it sets one at the music under the playhead (so moving a control while
// playing records automation). Pitch changes keep the music under the playhead in place.
bool setParamAt(Set& set, Id clipId, Param param, double value, double playheadSeconds);

// --- Keeping the rest of the mix in step ---

// Applies `change` (an edit of clip `clipId`, such as a nudge or a pitch change), then moves the
// clips after it so the mix stays as it was:
// - A clip that mixes in while a moved clip is playing keeps its music on that clip's music: it
//   is moved, and its pitch changed by the same ratio (to the nearest step, automation too), so
//   their beats stay together through the transition.
// - A clip that starts after a gap keeps the gap.
// Clips that mix into a follower follow in turn. Clips starting before the edited one stay put.
bool withFollowers(Set& set, Id clipId, const std::function<bool(Set&)>& change);

bool setNumDecks(Set& set, int numDecks);

// Corrections to the analysis; nullopt goes back to the detected values.
bool setTrackGrid(Set& set, Id trackId, std::optional<BeatGrid> grid);
bool setTrackKey(Set& set, Id trackId, std::optional<MusicalKey> key);

} // namespace vmp::edit

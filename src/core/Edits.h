#pragma once

#include "core/Set.h"

#include <functional>
#include <optional>
#include <vector>

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

// --- Recording mixer moves ---

// A control's position at a moment of a recording pass (timeline seconds).
struct RecordedMove
{
    double seconds = 0.0;
    double value = 0.0;
};

// How closely a recorded pass is redrawn, as a fraction of the control's range.
inline constexpr double kRecordingTolerance = 0.01;
// When a pass ends inside its clip, the control glides back to its earlier values over this long.
inline constexpr double kRecordingReturnSeconds = 0.05;

// The fewest moves that redraw `moves` (sorted by time) within `tolerance`: a move is dropped when
// the straight line between the moves kept either side passes within `tolerance` of its value
// (Ramer-Douglas-Peucker on the value error). The first and last are always kept.
std::vector<RecordedMove> thinMoves(const std::vector<RecordedMove>& moves, double tolerance);

// Writes a pass recorded on a mixer control (not pitch) of a playing clip, "latched": from the
// first move to `endSeconds` the control follows the moves and holds its last value between and
// after them. The keypoints in that stretch are replaced by the thinned moves; after it, unless
// the clip ends first, the control glides back to its earlier values. A pass from the needle drop
// sets the starting value.
bool writeRecordedPass(Set& set, Id clipId, Param param, const std::vector<RecordedMove>& moves, double endSeconds);

// A control's position after a trim amount (the mixer model's law, MixerChannel::trimmed).
using TrimLaw = std::function<double(Param param, double position, double amount)>;

// The trim amount at `seconds` in a pass of trim amounts: linear between moves, holding the last
// (and the first, before it).
double amountAt(const std::vector<RecordedMove>& moves, double seconds);

// Writes a pass of trim amounts on a mixer control (not pitch) of a playing clip: from the first
// move to `endSeconds`, the control's existing curve is adjusted by the amount (latched like
// writeRecordedPass), resampled finely enough to keep its shape, thinned and written in its place;
// after it the curve is as it was.
bool writeTrimmedPass(Set& set, Id clipId, Param param, const std::vector<RecordedMove>& amounts, double endSeconds,
                      const TrimLaw& law);

// Adjusts a mixer control's automation (not pitch) over the whole clip by a trim amount: the
// starting value and every keypoint, so the curve keeps its keypoints and shape.
bool trimAutomation(Set& set, Id clipId, Param param, double amount, const TrimLaw& law);

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

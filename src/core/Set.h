#pragma once

#include "core/Musical.h"
#include "core/PitchCurve.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vmp
{

using Id = int64_t;
inline constexpr int kMaxDecks = 4;

// Minimum time to swap records on one deck: lift the needle, change the record, find the cue.
// An estimate; a gap shorter than this is flagged, not forbidden.
inline constexpr double kMinChangeoverSeconds = 5.0;

// What the record is pressed at. Purely a label: the audio file doesn't say, and the user
// often doesn't know.
enum class PressedSpeed
{
    Unknown,
    Rpm33,
    Rpm45
};

// One record (side/track) available to the set, backed by an audio file.
struct Track
{
    Id id = 0;
    std::string path; // absolute, UTF-8
    std::string title;
    int64_t frames = 0; // length of the decoded file
    double sampleRate = 44100.0;
    PressedSpeed pressed = PressedSpeed::Unknown;
    // The DJ's corrections. When unset, the analysis results for the file are used.
    std::optional<BeatGrid> grid;
    std::optional<MusicalKey> key;

    bool operator==(const Track&) const = default;
};

// Mixer channel controls, stored as positions (0..1) as on the hardware rather than as gains.
// The mixer model decides what a position sounds like, so a set planned with one mixer can be
// played back through another. EQ and trim knobs: 0.5 is the centre detent.
struct ChannelSettings
{
    double fader = 1.0;
    double high = 0.5;
    double mid = 0.5;
    double low = 0.5;
    double trim = 0.5; // the input gain, per clip so each record keeps its own level; not automated
    double color = 0.5; // Sound Color FX knob; for now a filter: low-pass to the left, high-pass to the right

    bool operator==(const ChannelSettings&) const = default;
};

// The controls that can be automated during a clip.
enum class Param
{
    Pitch,  // in kPitchStepPercent steps
    Volume, // channel fader position, 0..1
    High,   // EQ knob positions, 0..1, centre 0.5
    Mid,
    Low,
    Color // 0..1, centre 0.5 (off)
};
inline constexpr int kNumParams = 6;

struct ParamRange
{
    double min, max;
};
ParamRange paramRange(Param param);
const char* paramName(Param param);

// A keypoint: the control's value at a position in the music. Values slide linearly between
// consecutive keypoints.
struct AutomationPoint
{
    int64_t sourceFrame = 0;
    double value = 0.0;

    bool operator==(const AutomationPoint&) const = default;
};

// The record's speed during a deck action, as a factor of the speed the pitch sets: 1 untouched,
// 0 held still, above 1 pushed. At `seconds` of timeline time since the action began; linear
// between points.
struct SpeedPoint
{
    double seconds = 0.0;
    double factor = 1.0;

    bool operator==(const SpeedPoint&) const = default;
};

// Something the DJ did to the record while it played, other than the mixer and the pitch fader,
// kept as a lane of its own and anchored to the music (like keypoints) at `sourceFrame`. Each is a
// short curve of the record's speed (speedShape) that multiplies the pitch automation's: the pitch
// stays on its steps, and an action stays the size it was played whatever the pitch around it.
// Kinds so far:
// - Nudge: from the nudge keys. A hand pushing the record ahead or holding it back, as a set
//   shape: in quickly to `speed` faster or slower, then easing off, until the music after it has
//   moved by `seconds` of timeline time (negative pushes it earlier, positive drags it later).
// - Touch: a hand on the record, measured: `touch` is how fast it went. Held still, it's 0.
// (Beat jumps, loops and other digital deck moves would be further kinds.)
struct DeckAction
{
    enum class Kind
    {
        Nudge,
        Touch
    };

    int64_t sourceFrame = 0;
    Kind kind = Kind::Nudge;
    double seconds = 0.0;             // Nudge
    double speed = 0.1;               // Nudge: kNudgeSpeed, within kMinNudgeSpeed..kMaxNudgeSpeed
    std::vector<SpeedPoint> touch;    // Touch: starts at 1 at 0 s and ends at 1
    bool linked = false;              // written by edit::solveLinks to keep a BeatLink, not by hand

    bool operator==(const DeckAction&) const = default;
};

// How much faster or slower a nudge plays the record at its peak, as a fraction of its speed: by
// default the same as the deck's own slewing when the playhead moves a little (Deck). The DJ can
// change it afterwards: gentler and longer, or sharper.
inline constexpr double kNudgeSpeed = 0.1;
inline constexpr double kMinNudgeSpeed = 0.01;
inline constexpr double kMaxNudgeSpeed = 0.5;
// The nudge shape: in over this fraction of its length, at its peak until this fraction, then
// easing off. A guess at a hand; measured touches will tell.
inline constexpr double kNudgeAttack = 0.3;
inline constexpr double kNudgeEaseFrom = 0.6;
// How long a nudge of `seconds` lasts on the timeline at a peak of `speed`.
double nudgeLengthSeconds(double seconds, double speed = kNudgeSpeed);
// A record let go gets back up to speed this fast (the platter keeps turning under the slipmat).
inline constexpr double kTouchReleaseSeconds = 0.03;
// Fastest a hand can push a record, as a factor of its speed.
inline constexpr double kMaxTouchFactor = 3.0;

// The action's curve of speed factors, from 1 at 0 s back to 1 at the end.
std::vector<SpeedPoint> speedShape(const DeckAction& action);
// How long it lasts on the timeline.
double actionLengthSeconds(const DeckAction& action);
// How far it moves the music after it on the timeline: positive later, negative earlier.
double actionOffsetSeconds(const DeckAction& action);

// A track played on a deck: needle drop at inFrame at timeline time startSeconds, played until
// outFrame (needle up or fader closed).
struct Clip
{
    Id id = 0;
    Id trackId = 0;
    int deck = 0; // zero-based
    double startSeconds = 0.0;
    int64_t inFrame = 0;
    int64_t outFrame = 0;
    // Starting values: a fixed point at the needle drop for each control. New clips start from
    // the defaults (0% pitch, full volume, EQ centred).
    double pitchSteps = 0; // whole steps, except after trimming into a pitch slide
    ChannelSettings mixer;
    // Keypoints per Param after the starting point, sorted by sourceFrame. Two points at one
    // frame make a jump. Keypoints before the needle drop are kept but ignored.
    std::array<std::vector<AutomationPoint>, kNumParams> automation;
    // Sorted by sourceFrame. Actions before the needle drop are kept but ignored.
    std::vector<DeckAction> actions;

    const std::vector<AutomationPoint>& points(Param p) const { return automation[static_cast<size_t>(p)]; }
    std::vector<AutomationPoint>& points(Param p) { return automation[static_cast<size_t>(p)]; }

    bool operator==(const Clip&) const = default;
};

// The clip's starting value for a control.
double baseValue(const Clip& clip, Param param);
// A control's value at a position in the music: linear from the starting value at the needle
// drop through the keypoints, holding the last. (Pitch slides are linear in time, which
// ClipTiming models exactly; this is the source-linear approximation, fine for display.)
double valueAt(const Clip& clip, Param param, double sourceFrame);
ChannelSettings mixerAt(const Clip& clip, double sourceFrame);

// Two moments in the music that the DJ wants heard together: `followerFrame` of the follower
// plays at the same time as `leaderFrame` of the leader. The follower is the record mixed in
// later; edit::solveLinks keeps its links by placing it, setting its pitch (with two or more) and
// nudging it before each link, as a DJ would by ear.
struct BeatLink
{
    Id id = 0;
    Id leader = 0;
    int64_t leaderFrame = 0;
    Id follower = 0;
    int64_t followerFrame = 0;

    bool operator==(const BeatLink&) const = default;
};

struct Set
{
    std::string name;
    int numDecks = 2;
    std::vector<Track> tracks;
    std::vector<Clip> clips;
    std::vector<BeatLink> links;
    Id nextId = 1;

    Id allocateId() { return nextId++; }
    const Track* findTrack(Id id) const;
    const Clip* findClip(Id id) const;

    bool operator==(const Set&) const = default;
};

// Timeline <-> source mapping for one clip, including its pitch automation and nudges.
//
// The pitch curve maps "pitch time" (seconds since the needle drop, as if there were no nudges) to
// the music; nudges then warp pitch time into timeline time, piecewise linearly: each one, from
// where its music plays, runs pitch time a little slower or faster than the timeline until the
// music after it has moved by its amount. Held still, pitch time stops (a step in the warp).
class ClipTiming
{
public:
    ClipTiming(const Clip& clip, const Track& track);

    double startSeconds() const { return start_; }
    double endSeconds() const { return end_; }

    double speedAt(double timelineSeconds) const;
    // The speed set by the pitch automation alone, without nudges.
    double pitchSpeedAt(double timelineSeconds) const;
    double sourceFrameAt(double timelineSeconds) const; // fractional frames
    double timelineSecondsAt(double sourceFrame) const;

private:
    // Pitch time <-> timeline time since the needle drop; how fast pitch time runs at timeline time
    // w (0 while held).
    double warp(double p) const;
    double unwarp(double w) const;
    double rateAt(double w) const;

    double start_;
    double sampleRate_;
    double inFrame_;
    PitchCurve curve_;
    // The nudges' warp: breakpoints in pitch time and timeline time. Identity before the first,
    // shifted by the sum of the actions after the last; held still, breakpoints share a pitch time.
    // Empty when there are none.
    std::vector<double> pitchTimes_, warpedTimes_;
    double end_;
};

struct Issue
{
    enum class Severity
    {
        Warning, // possible on real decks but risky, or needs something you might not have
        Error    // can't be done on real decks, or the set is inconsistent
    };

    Severity severity;
    std::string message;
    std::vector<Id> clipIds;
};

// Checks the set against what two to four 1200s can physically do.
std::vector<Issue> validate(const Set& set);

} // namespace vmp

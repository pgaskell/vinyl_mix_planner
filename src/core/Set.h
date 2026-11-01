#pragma once

#include "core/Musical.h"
#include "core/PitchCurve.h"

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

// A fader move during a clip, anchored to a position in the music.
struct PitchMove
{
    int64_t sourceFrame = 0;     // where in the track the move starts
    double durationSeconds = 0;  // real time the move takes; 0 is a jump
    int targetSteps = 0;

    bool operator==(const PitchMove&) const = default;
};

// Mixer channel controls, stored as positions (0..1) as on the hardware rather than as gains.
// The mixer model decides what a position sounds like, so a set planned with one mixer can be
// played back through another. EQ knobs: 0.5 is the centre detent.
struct ChannelSettings
{
    double fader = 1.0;
    double high = 0.5;
    double mid = 0.5;
    double low = 0.5;

    bool operator==(const ChannelSettings&) const = default;
};

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
    int pitchSteps = 0; // fader position at the needle drop
    std::vector<PitchMove> pitchMoves;
    ChannelSettings mixer; // channel settings when the clip starts

    bool operator==(const Clip&) const = default;
};

struct Set
{
    std::string name;
    int numDecks = 2;
    std::vector<Track> tracks;
    std::vector<Clip> clips;
    Id nextId = 1;

    Id allocateId() { return nextId++; }
    const Track* findTrack(Id id) const;
    const Clip* findClip(Id id) const;

    bool operator==(const Set&) const = default;
};

// Timeline <-> source mapping for one clip. Throws std::invalid_argument for invalid pitch moves.
class ClipTiming
{
public:
    ClipTiming(const Clip& clip, const Track& track);

    double startSeconds() const { return start_; }
    double endSeconds() const { return end_; }

    double speedAt(double timelineSeconds) const;
    double sourceFrameAt(double timelineSeconds) const; // fractional frames
    double timelineSecondsAt(double sourceFrame) const;

private:
    double start_;
    double sampleRate_;
    double inFrame_;
    PitchCurve curve_;
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

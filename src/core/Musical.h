#pragma once

#include <optional>
#include <string>

namespace vmp
{

// Constant-tempo beat grid in source time: beat k (any integer) is at
// firstBeatSeconds + k * periodSeconds(). Counting from firstBeatSeconds, every 4th beat is a
// downbeat and every 64th starts a 16-bar phrase.
inline constexpr int kBeatsPerBar = 4;
inline constexpr int kBeatsPerPhrase = 64;

struct BeatGrid
{
    double bpm = 120.0;
    double firstBeatSeconds = 0.0;

    double periodSeconds() const { return 60.0 / bpm; }
    bool operator==(const BeatGrid&) const = default;
};

struct MusicalKey
{
    int tonic = 0; // 0 = C ... 11 = B
    bool minor = false;

    MusicalKey transposed(int semitones) const;
    std::string name() const;    // "C", "F#m", "Bbm"
    std::string camelot() const; // "8B", "8A"
    int camelotNumber() const;   // 1..12

    // Accepts names ("Am", "C#", "Bbm", "Gb") and Camelot codes ("8A", "12B").
    static std::optional<MusicalKey> parse(const std::string& text);

    bool operator==(const MusicalKey&) const = default;
};

// How a key sounds when the record plays at `speed`: the nearest key, and how far the
// actual pitch is from it (cents, -50..+50).
struct PitchedKey
{
    MusicalKey key;
    double cents;
};
PitchedKey pitchedKey(MusicalKey original, double speed);

// Mixing compatibility on the Camelot wheel.
enum class Harmony
{
    Same,       // same key
    Compatible, // relative major/minor, or one step around the wheel
    Clash
};
Harmony harmony(MusicalKey a, MusicalKey b);

} // namespace vmp

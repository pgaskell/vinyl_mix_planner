#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vmp
{

// Beat grid in source time. Counting from beat 0, every 4th beat is a downbeat and every 64th
// starts a 16-bar phrase.
//
// Without pins it is steady: beat k is at firstBeatSeconds + k * periodSeconds(). Pins say where
// particular beats really are (a record that drifts, a tape splice): between two pins the beats
// are spread evenly, and before the first pin and after the last they carry on at `bpm`. Beat 0
// stays the downbeat, and firstBeatSeconds is always where it is.
inline constexpr int kBeatsPerBar = 4;
inline constexpr int kBeatsPerPhrase = 64;

struct BeatPin
{
    int64_t beat = 0;
    double seconds = 0.0;

    bool operator==(const BeatPin&) const = default;
};

struct BeatGrid
{
    double bpm = 120.0;
    double firstBeatSeconds = 0.0;
    std::vector<BeatPin> pins; // by beat; seconds increase with them

    BeatGrid() = default;
    BeatGrid(double bpm_, double firstBeatSeconds_, std::vector<BeatPin> pins_ = {})
        : bpm(bpm_), firstBeatSeconds(firstBeatSeconds_), pins(std::move(pins_))
    {
    }

    // The nominal beat length, at `bpm`.
    double periodSeconds() const { return 60.0 / bpm; }
    // Where beat `beat` (fractional) is, and which beat (fractional) is at `seconds`.
    double secondsOfBeat(double beat) const;
    double beatAt(double seconds) const;
    // How long a beat is around `seconds`.
    double periodAt(double seconds) const;

    // Beat `beat` moved to `seconds`, kept between its neighbouring pins (so the beats there stay
    // within half to double the nominal length). The first pin of a grid also pins beat 0 where
    // it is, so the downbeat stays put and only the beats between it and the new pin spread out.
    BeatGrid pinned(int64_t beat, double seconds) const;
    BeatGrid unpinned(int64_t beat) const;
    const BeatPin* findPin(int64_t beat) const;

    // The whole grid moved by `seconds`; beat `beat` counted as beat 0 from now on; twice or half
    // the tempo (halving drops the pins on odd beats, which are no longer beats).
    BeatGrid shifted(double seconds) const;
    BeatGrid renumbered(int64_t beat) const;
    BeatGrid doubled() const;
    // Beat 0 (a bar line) at `seconds`. With pins it goes on the nearest beat: the pins place them.
    BeatGrid withDownbeatAt(double seconds) const;
    BeatGrid halved() const;
    // Every pin finite, beats and seconds strictly increasing.
    bool valid() const;

    bool operator==(const BeatGrid&) const = default;

private:
    // firstBeatSeconds kept at beat 0; a lone pin becomes a steady grid.
    BeatGrid tidied() const;
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

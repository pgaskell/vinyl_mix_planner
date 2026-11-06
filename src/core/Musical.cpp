#include "core/Musical.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace vmp
{

namespace
{
const char* const kMajorNames[] = {"C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
const char* const kMinorNames[] = {"Cm", "C#m", "Dm", "Ebm", "Em", "Fm", "F#m", "Gm", "G#m", "Am", "Bbm", "Bm"};

int wrap12(int n)
{
    return ((n % 12) + 12) % 12;
}
} // namespace

double BeatGrid::secondsOfBeat(double beat) const
{
    const double period = periodSeconds();
    if (pins.empty())
        return firstBeatSeconds + beat * period;
    if (beat <= static_cast<double>(pins.front().beat))
        return pins.front().seconds + (beat - static_cast<double>(pins.front().beat)) * period;
    if (beat >= static_cast<double>(pins.back().beat))
        return pins.back().seconds + (beat - static_cast<double>(pins.back().beat)) * period;
    const auto next = std::upper_bound(pins.begin(), pins.end(), beat,
                                       [](double b, const BeatPin& pin) { return b < static_cast<double>(pin.beat); });
    const auto prev = next - 1;
    const double f = (beat - static_cast<double>(prev->beat)) / static_cast<double>(next->beat - prev->beat);
    return prev->seconds + f * (next->seconds - prev->seconds);
}

double BeatGrid::beatAt(double seconds) const
{
    const double period = periodSeconds();
    if (pins.empty())
        return (seconds - firstBeatSeconds) / period;
    if (seconds <= pins.front().seconds)
        return static_cast<double>(pins.front().beat) + (seconds - pins.front().seconds) / period;
    if (seconds >= pins.back().seconds)
        return static_cast<double>(pins.back().beat) + (seconds - pins.back().seconds) / period;
    const auto next = std::upper_bound(pins.begin(), pins.end(), seconds,
                                       [](double s, const BeatPin& pin) { return s < pin.seconds; });
    const auto prev = next - 1;
    const double f = (seconds - prev->seconds) / (next->seconds - prev->seconds);
    return static_cast<double>(prev->beat) + f * static_cast<double>(next->beat - prev->beat);
}

double BeatGrid::periodAt(double seconds) const
{
    if (pins.size() < 2 || seconds <= pins.front().seconds || seconds >= pins.back().seconds)
        return periodSeconds();
    const auto next = std::upper_bound(pins.begin(), pins.end(), seconds,
                                       [](double s, const BeatPin& pin) { return s < pin.seconds; });
    const auto prev = next - 1;
    return (next->seconds - prev->seconds) / static_cast<double>(next->beat - prev->beat);
}

const BeatPin* BeatGrid::findPin(int64_t beat) const
{
    const auto it = std::find_if(pins.begin(), pins.end(), [beat](const BeatPin& pin) { return pin.beat == beat; });
    return it != pins.end() ? &*it : nullptr;
}

BeatGrid BeatGrid::pinned(int64_t beat, double seconds) const
{
    BeatGrid grid = *this;
    if (grid.pins.empty() && beat != 0)
        grid.pins.push_back({0, firstBeatSeconds});
    std::erase_if(grid.pins, [beat](const BeatPin& pin) { return pin.beat == beat; });
    const auto next = std::lower_bound(grid.pins.begin(), grid.pins.end(), beat,
                                       [](const BeatPin& pin, int64_t b) { return pin.beat < b; });
    // Beats between the new pin and its neighbours at least half their nominal length.
    const double half = 0.5 * periodSeconds();
    double lower = -1e12;
    double upper = 1e12;
    if (next != grid.pins.begin())
        lower = (next - 1)->seconds + static_cast<double>(beat - (next - 1)->beat) * half;
    if (next != grid.pins.end())
        upper = next->seconds - static_cast<double>(next->beat - beat) * half;
    if (!(lower < upper) || !std::isfinite(seconds))
        return *this;
    grid.pins.insert(next, {beat, std::clamp(seconds, lower, upper)});
    return grid.tidied();
}

BeatGrid BeatGrid::unpinned(int64_t beat) const
{
    BeatGrid grid = *this;
    std::erase_if(grid.pins, [beat](const BeatPin& pin) { return pin.beat == beat; });
    return grid.tidied();
}

BeatGrid BeatGrid::shifted(double seconds) const
{
    BeatGrid grid = *this;
    grid.firstBeatSeconds += seconds;
    for (auto& pin : grid.pins)
        pin.seconds += seconds;
    return grid;
}

BeatGrid BeatGrid::renumbered(int64_t beat) const
{
    BeatGrid grid = *this;
    grid.firstBeatSeconds = secondsOfBeat(static_cast<double>(beat));
    for (auto& pin : grid.pins)
        pin.beat -= beat;
    return grid;
}

BeatGrid BeatGrid::withDownbeatAt(double seconds) const
{
    if (pins.empty())
        return {bpm, seconds};
    return renumbered(static_cast<int64_t>(std::llround(beatAt(seconds))));
}

BeatGrid BeatGrid::doubled() const
{
    BeatGrid grid = *this;
    grid.bpm *= 2.0;
    for (auto& pin : grid.pins)
        pin.beat *= 2;
    return grid;
}

BeatGrid BeatGrid::halved() const
{
    BeatGrid grid = *this;
    grid.bpm /= 2.0;
    std::erase_if(grid.pins, [](const BeatPin& pin) { return pin.beat % 2 != 0; });
    for (auto& pin : grid.pins)
        pin.beat /= 2;
    return grid.tidied();
}

BeatGrid BeatGrid::tidied() const
{
    BeatGrid grid = *this;
    grid.firstBeatSeconds = secondsOfBeat(0.0);
    if (grid.pins.size() == 1)
        grid.pins.clear(); // one pin says no more than where the beats are: a steady grid
    return grid;
}

bool BeatGrid::valid() const
{
    if (!std::isfinite(bpm) || !std::isfinite(firstBeatSeconds))
        return false;
    for (size_t i = 0; i < pins.size(); ++i)
    {
        if (!std::isfinite(pins[i].seconds))
            return false;
        if (i > 0 && (pins[i].beat <= pins[i - 1].beat || pins[i].seconds <= pins[i - 1].seconds))
            return false;
    }
    return true;
}

MusicalKey MusicalKey::transposed(int semitones) const
{
    return {wrap12(tonic + semitones), minor};
}

std::string MusicalKey::name() const
{
    return minor ? kMinorNames[wrap12(tonic)] : kMajorNames[wrap12(tonic)];
}

int MusicalKey::camelotNumber() const
{
    // Each fifth up is one step round the wheel; C major is 8B and its relative A minor is 8A.
    const int major = minor ? wrap12(tonic + 3) : wrap12(tonic);
    return wrap12(major * 7 + 7) + 1;
}

std::string MusicalKey::camelot() const
{
    return std::to_string(camelotNumber()) + (minor ? "A" : "B");
}

std::optional<MusicalKey> MusicalKey::parse(const std::string& text)
{
    if (text.empty())
        return std::nullopt;

    // Camelot: number then A or B.
    if (std::isdigit(static_cast<unsigned char>(text[0])))
    {
        if (text.size() > 3)
            return std::nullopt;
        size_t used = 0;
        const int number = std::stoi(text, &used);
        if (number < 1 || number > 12 || used + 1 != text.size())
            return std::nullopt;
        const char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(text[used])));
        if (letter != 'A' && letter != 'B')
            return std::nullopt;
        // Invert camelotNumber() for the major key, then move to the relative minor if needed.
        const int major = wrap12((number - 8) * 7);
        return letter == 'B' ? MusicalKey{major, false} : MusicalKey{wrap12(major - 3), true};
    }

    static const int kLetters[] = {9, 11, 0, 2, 4, 5, 7}; // A..G
    const char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    if (letter < 'A' || letter > 'G')
        return std::nullopt;
    int tonic = kLetters[letter - 'A'];
    size_t i = 1;
    if (i < text.size() && (text[i] == '#' || text[i] == 'b'))
        tonic += text[i++] == '#' ? 1 : -1;
    bool minor = false;
    if (i < text.size() && text[i] == 'm')
    {
        minor = true;
        ++i;
    }
    if (i != text.size())
        return std::nullopt;
    return MusicalKey{wrap12(tonic), minor};
}

PitchedKey pitchedKey(MusicalKey original, double speed)
{
    const double semitones = 12.0 * std::log2(speed);
    const auto nearest = static_cast<int>(std::lround(semitones));
    return {original.transposed(nearest), (semitones - nearest) * 100.0};
}

Harmony harmony(MusicalKey a, MusicalKey b)
{
    if (a == b)
        return Harmony::Same;
    const int na = a.camelotNumber(), nb = b.camelotNumber();
    if (na == nb)
        return Harmony::Compatible; // relative major/minor
    const int steps = std::abs(na - nb);
    if (a.minor == b.minor && (steps == 1 || steps == 11))
        return Harmony::Compatible;
    return Harmony::Clash;
}

} // namespace vmp

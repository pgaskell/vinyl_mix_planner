#include "core/Musical.h"

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

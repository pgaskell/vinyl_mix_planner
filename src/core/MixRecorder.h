#pragma once

#include "core/Edits.h"
#include "core/Set.h"

#include <array>
#include <optional>
#include <vector>

namespace vmp
{

// What moving a mixer control while playing does (stopped, it edits the set in every mode). The
// names are the DAWs', though the modes differ a little:
// - Override: you hear it, nothing is written (practice);
// - Latch: its positions replace the automation from where you took hold of it until playback
//   stops (or the record ends);
// - Trim: it adjusts the automation that's there (MixerChannel::trimmed) from where you took hold of
//   it until playback stops (or the record ends), latched like Latch; a drag sets the amount;
// - Trim All: the same over the record's whole curve for that control.
// (A Write mode, recording every control of a MIDI mixer from the start, will come with MIDI input.)
enum class AutomationMode
{
    Override,
    Latch,
    Trim,
    TrimAll
};

// Whether a mode's moves are trim amounts rather than positions.
inline bool isTrim(AutomationMode mode)
{
    return mode == AutomationMode::Trim || mode == AutomationMode::TrimAll;
}

// Collects the mixer moves made while playing, until they're written into the set. Controls
// latch: once moved, a control stays where it was left until playback stops, jumps or its clip
// ends, as a real fader or knob does; until then the engine plays the latched values (latched(),
// latchedTrim()), since the set doesn't have them yet. Each pass keeps the mode it was started in,
// so switching modes never changes what's heard: only the next control taken hold of uses the new
// one. Moves a little behind the playhead are thinned as recording goes on (settle()), so a long
// pass stays small and is drawn as it will end up.
//
// A pass records positions (Override: dropped; Latch: edit::writeRecordedPass) or trim amounts
// (Trim: edit::writeTrimmedPass; Trim All: edit::trimAutomation with the last move); the engine
// plays a trim pass as a scale on the set's values.
//
// One recorder for every input: the on-screen mixer now, a hardware mixer's MIDI later.
class MixRecorder
{
public:
    // A recorded control on one deck: its moves so far on the clip playing there.
    struct Pass
    {
        Id clip = 0;
        Param param = Param::Volume;
        AutomationMode mode = AutomationMode::Latch; // in the trim modes, moves are trim amounts
        std::vector<edit::RecordedMove> moves; // the first `settled` already thinned
        size_t settled = 0;
        double clipEnd = 0.0; // timeline seconds
        double end = 0.0;     // where the pass stops, once finished
    };

    // A mixer control (not pitch) set to `value` (a position, or a trim amount in Trim) on `deck` at
    // timeline time `seconds`, while `clip` (ending at `clipEnd`) plays there. Taking hold of a
    // control counts as a move to where it is. A pass on another clip of the same deck, or in
    // another mode, is finished at `seconds`.
    void move(int deck, Id clip, double clipEnd, Param param, double value, double seconds,
              AutomationMode mode = AutomationMode::Latch);

    // Where a control on `deck` is latched, if it's holding a position (Override or Latch).
    std::optional<double> latched(int deck, Param param) const;
    // The trim amount a control on `deck` is latched at, if it's being trimmed.
    std::optional<double> latchedTrim(int deck, Param param) const;
    // The pass being recorded on a deck's control, if any (for drawing it).
    const Pass* pass(int deck, Param param) const;
    bool recording() const;

    // Thins each pass's moves before `seconds`.
    void settle(double seconds);

    // Finishes the passes whose clip has ended by `seconds`, at their clip's end.
    void finishEnded(double seconds);
    // Finishes every pass at `seconds` (playback stopped or jumped there, or recording was disarmed).
    void finishAll(double seconds);
    // The finished passes, to be written into the set; they're forgotten here.
    std::vector<Pass> takeFinished();

private:
    static size_t index(Param param) { return static_cast<size_t>(param) - static_cast<size_t>(Param::Volume); }
    void finish(std::optional<Pass>& pass, double seconds);

    static constexpr size_t kControls = kNumParams - 1; // all but pitch
    std::array<std::array<std::optional<Pass>, kControls>, kMaxDecks> passes_;
    std::vector<Pass> finished_;
};

} // namespace vmp

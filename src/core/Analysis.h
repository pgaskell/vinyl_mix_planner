#pragma once

#include "core/Musical.h"
#include "core/TrackAudio.h"

#include <optional>
#include <vector>

namespace vmp
{

// What analysis could work out about a track. Either part may be missing (silence, no steady
// beat, no clear key). Results are estimates for the DJ to check, not facts.
struct TrackAnalysis
{
    std::optional<BeatGrid> grid;
    std::optional<MusicalKey> key;

    bool operator==(const TrackAnalysis&) const = default;
};

// Runs on a background thread; takes a fraction of a second per track.
TrackAnalysis analyseTrack(const TrackAudio& audio);

// Building blocks, exposed for tests. Both take mono audio.
// Assumes a constant tempo between 60 and 190 BPM, preferring tempos near 120.
std::optional<BeatGrid> detectBeatGrid(const std::vector<float>& mono, double sampleRate);
// Correlates the track's pitch-class profile with Krumhansl-Kessler key profiles.
std::optional<MusicalKey> detectKey(const std::vector<float>& mono, double sampleRate);

// Grids from the DJ's clicks, which are rough: the transients around them decide. Both take the
// analysis signal (analysisSignal) and seconds in the track.
// The grid at `bpm` with its downbeat moved onto the beat within kClickReach of `downbeat` that
// fits the music around it best (at `downbeat` if there's no clear beat there).
inline constexpr double kClickReach = 0.06;
BeatGrid fitDownbeat(const std::vector<float>& mono, double sampleRate, double bpm, double downbeat);
// The steady grid, with its downbeat near `downbeat` and a beat near `beat` (any beat before or
// after it), that best fits the transients over the few minutes around them. The closer the clicks,
// the less sure which beat `beat` is, so the more it is checked against the music; a click on the
// next beat settles the tempo's octave. Nothing when the clicks are under 0.2 s apart or there's
// no steady beat to fit.
std::optional<BeatGrid> fitBeatGrid(const std::vector<float>& mono, double sampleRate, double downbeat, double beat);

// Mixes to mono and decimates to roughly 11 kHz, which is plenty for both detectors.
std::vector<float> analysisSignal(const TrackAudio& audio, double& sampleRate);

} // namespace vmp

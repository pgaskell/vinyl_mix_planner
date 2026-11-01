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

// Mixes to mono and decimates to roughly 11 kHz, which is plenty for both detectors.
std::vector<float> analysisSignal(const TrackAudio& audio, double& sampleRate);

} // namespace vmp

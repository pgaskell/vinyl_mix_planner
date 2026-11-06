#pragma once

#include "core/TrackAudio.h"

#include <cstdint>
#include <vector>

typedef struct SRC_STATE_tag SRC_STATE;

namespace vmp
{

// One turntable's playback. The caller says which stretch of the record to play in each block
// (from the clip timing), and the deck resamples it into the block, so pitch and tempo move
// together like vinyl. Audio thread only, apart from prepare().
class Deck
{
public:
    // Requests within this distance of the deck's position are reached by slewing; beyond, it jumps.
    static constexpr double kSlewWindowSeconds = 0.1;
    // How far outside [startFrame, endFrame) a render() may read, in seconds of the track: the slew
    // window plus the resampler's look-ahead (tens of frames). The engine only renders a clip whose
    // audio is loaded this far around the requested range. Raising the slew window raises this.
    static constexpr double kReadMarginSeconds = kSlewWindowSeconds + 0.05;

    Deck();
    ~Deck();
    Deck(const Deck&) = delete;
    Deck& operator=(const Deck&) = delete;

    // Not real-time.
    void prepare(double deviceSampleRate, int maxBlockFrames);

    // Plays source frames [startFrame, endFrame) of the track into numFrames output frames. Reads only
    // within kReadMarginSeconds of that range; if the audio there isn't loaded, the rest is silent.
    // If startFrame is close to where the deck actually is, it slews: it plays slightly faster
    // or slower until it catches up, like nudging a platter, so small edits during playback
    // (nudges, pitch changes) don't click. Anything further away, or another track, jumps.
    // An empty range near where the deck is (the record held still) is silence, and the deck
    // stays put.
    void render(const TrackAudio* track, double startFrame, double endFrame, float* left, float* right,
                int numFrames);

    void renderSilence(float* left, float* right, int numFrames);

private:
    void restart(const TrackAudio* track, double startFrame);

    SRC_STATE* src_ = nullptr;
    const TrackAudio* track_ = nullptr;
    int64_t readFrame_ = 0;     // next frame to feed the resampler
    double position_ = -1;      // source frame the deck has actually reached
    std::vector<float> scratch_; // interleaved resampler output
};

} // namespace vmp

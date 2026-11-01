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
    Deck();
    ~Deck();
    Deck(const Deck&) = delete;
    Deck& operator=(const Deck&) = delete;

    // Not real-time.
    void prepare(double deviceSampleRate, int maxBlockFrames);

    // Plays source frames [startFrame, endFrame) of the track into numFrames output frames.
    // If startFrame is close to where the deck actually is, it slews: it plays slightly faster
    // or slower until it catches up, like nudging a platter, so small edits during playback
    // (nudges, pitch changes) don't click. Anything further away, or another track, jumps.
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

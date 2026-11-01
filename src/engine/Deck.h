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
    // A call that continues where the last one ended keeps the resampler running; anything else
    // (another track, a jump) restarts it at startFrame.
    void render(const TrackAudio* track, double startFrame, double endFrame, float* left, float* right,
                int numFrames);

    void renderSilence(float* left, float* right, int numFrames);

private:
    void restart(const TrackAudio* track, double startFrame);

    SRC_STATE* src_ = nullptr;
    const TrackAudio* track_ = nullptr;
    int64_t readFrame_ = 0;     // next frame to feed the resampler
    double expectedStart_ = -1; // where a continuing call would start
    std::vector<float> scratch_; // interleaved resampler output
};

} // namespace vmp

#pragma once

#include "core/Motor.h"

#include <atomic>
#include <cstdint>
#include <vector>

typedef struct SRC_STATE_tag SRC_STATE;

namespace vmp
{

// Decoded audio for one record side: interleaved stereo float, immutable once handed to a Deck.
// Whole-file in memory (~10 MB/minute at 44.1 kHz). Fine for a set; revisit if memory bites.
struct TrackAudio
{
    std::vector<float> interleaved;
    int64_t frames = 0;
    double sampleRate = 44100.0;
};

// One turntable. Speed changes resample the audio, so pitch and tempo move together like vinyl.
//
// Threading: setters are called from the message thread and are lock-free. render() runs on the
// audio thread. prepare() must not run concurrently with render().
class Deck
{
public:
    Deck();
    ~Deck();
    Deck(const Deck&) = delete;
    Deck& operator=(const Deck&) = delete;

    // --- Non-real-time -----------------------------------------------------
    void prepare(double deviceSampleRate, int maxBlockFrames);

    // Takes ownership. The audio thread swaps it in at the start of its next block and the old
    // track is freed later by collectGarbage(), so nothing is deallocated on the audio thread.
    void setTrack(TrackAudio* track);
    void collectGarbage();

    // --- Controls (message thread, lock-free) ------------------------------
    void setRunning(bool running) { running_.store(running); }
    bool isRunning() const { return running_.load(); }
    void setPitchSteps(int steps) { pitchSteps_.store(steps); }
    int pitchSteps() const { return pitchSteps_.load(); }
    void setMotorTimes(double startSeconds, double stopSeconds);
    void seek(int64_t frame) { seekRequest_.store(frame); }

    // Position in source frames, published once per block.
    int64_t positionFrames() const { return position_.load(); }

    // --- Audio thread ------------------------------------------------------
    void render(float* left, float* right, int numFrames);

private:
    void adoptPendingTrack();
    void renderChunk(float* left, float* right, int numFrames);

    std::atomic<TrackAudio*> pending_{nullptr};
    std::atomic<TrackAudio*> retired_{nullptr};
    TrackAudio* current_ = nullptr; // audio thread only

    std::atomic<bool> running_{false};
    std::atomic<int> pitchSteps_{0};
    std::atomic<double> startSeconds_{kMk2StartSeconds};
    std::atomic<double> stopSeconds_{kMk2StopSeconds};
    std::atomic<int64_t> seekRequest_{-1};
    std::atomic<int64_t> position_{0};

    // Audio thread state.
    SRC_STATE* src_ = nullptr;
    MotorModel motor_;
    int64_t readFrame_ = 0;
    bool wasMoving_ = false;
    double deviceRate_ = 48000.0;
    std::vector<float> scratch_; // interleaved resampler output
};

} // namespace vmp

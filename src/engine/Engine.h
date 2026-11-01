#pragma once

#include "engine/Deck.h"
#include "engine/Glitches.h"
#include "engine/MixerChannel.h"
#include "engine/RtHandoff.h"
#include "engine/Schedule.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace vmp
{

enum class OutputMode
{
    InternalMix, // decks through the internal mixer, summed to one stereo pair (laptop use)
    DirectOut    // each deck dry to its own stereo pair, mixing done on a hardware mixer
};

// Plays the set's schedule: deck N plays lane N. Knows nothing about JUCE: the host passes a
// map from physical device channel to the index in the buffer array the driver hands us,
// because drivers only hand over the channels that are enabled.
class Engine
{
public:
    static constexpr int kNoOutput = -1;

    Engine();

    // --- Message thread, lock-free -----------------------------------------
    void setSchedule(std::unique_ptr<Schedule> schedule) { schedule_.set(std::move(schedule)); }
    // Whether the audio thread is still playing an older schedule than the last one set.
    bool schedulePending() const { return schedule_.hasPending(); }

    void play() { playing_.store(true); }
    void pause() { playing_.store(false); }
    bool isPlaying() const { return playing_.load(); }
    void seek(double timelineSeconds) { seekRequest_.store(timelineSeconds); }
    double playheadSeconds() const { return playhead_.load(); }
    // Where audio is about to be needed: a seek not yet taken up, otherwise the playhead.
    double prefetchTarget() const;

    // Incremented as each render() begins and ends: odd while rendering. Memory the audio thread
    // may be reading (streamed chunks) can be reused once this has been seen even, or has changed,
    // after the memory was unpublished.
    uint64_t renderCounter() const { return renderCounter_.load(); }

    // After play or a seek, how long the engine holds the playhead while waiting for streamed
    // audio before playing anyway (and reporting underruns).
    static constexpr double kPrerollSeconds = 0.5;

    void setOutputMode(OutputMode mode) { mode_.store(mode); }
    OutputMode outputMode() const { return mode_.load(); }

    // Outputs are given as the first physical channel of a stereo pair, or kNoOutput.
    void setDeckOutput(int deckIndex, int firstPhysicalChannel);
    int deckOutput(int deckIndex) const;
    void setMasterOutput(int firstPhysicalChannel) { masterOutput_.store(firstPhysicalChannel); }
    int masterOutput() const { return masterOutput_.load(); }

    // Frees schedules the audio thread has finished with.
    void collectGarbage() { schedule_.collectGarbage(); }

    // Glitches the engine noticed while playing, oldest first. Returns false when there are no more.
    bool popGlitch(Glitch& glitch) { return glitches_.pop(glitch); }
    // Glitches lost because nobody collected them in time, since the last call.
    uint32_t takeDroppedGlitches() { return glitches_.takeDropped(); }
    // The largest fraction of a block's duration spent rendering it, since the last call.
    float takePeakLoad() { return peakLoad_.exchange(0.0f); }

    // Replaces the clock used to time blocks (nanoseconds), for tests. Not real-time safe to change
    // while running.
    using Clock = int64_t (*)();
    void setClock(Clock clock) { clock_ = clock; }

    // --- Non-real-time -----------------------------------------------------
    // physicalToBuffer[ch] is the buffer index for physical output ch, or -1 if disabled.
    void prepare(double sampleRate, int maxBlockFrames, std::vector<int> physicalToBuffer);

    // --- Audio thread ------------------------------------------------------
    void render(float* const* outputs, int numOutputs, int numFrames);

private:
    void renderChunk(const Schedule* schedule, bool playing, float* const* outputs, int numOutputs, int offset,
                     int numFrames);
    void renderLane(const std::vector<ScheduledClip>& lane, size_t deck, double start, int numFrames);
    bool audioLoaded(const Schedule* schedule, double start, int numFrames) const;
    float* outputFor(float* const* outputs, int numOutputs, int physicalChannel) const;

    RtHandoff<Schedule> schedule_;
    std::atomic<bool> playing_{false};
    std::atomic<double> seekRequest_;
    std::atomic<double> playhead_{0.0}; // published once per block
    std::atomic<OutputMode> mode_{OutputMode::InternalMix};
    std::array<std::atomic<int>, kMaxDecks> deckOutputs_;
    std::atomic<int> masterOutput_{0};
    SpscQueue<Glitch, 256> glitches_;
    std::atomic<uint64_t> renderCounter_{0};
    std::atomic<float> peakLoad_{0.0f};
    Clock clock_;

    // Audio thread state.
    std::array<std::unique_ptr<Deck>, kMaxDecks> decks_;
    std::array<MixerChannel, kMaxDecks> channels_;
    std::array<ChannelSettings, kMaxDecks> laneMixer_; // settings of the clip last heard on each lane
    std::array<Id, kMaxDecks> silentClip_{};           // clip already reported as not loaded, per lane
    std::array<Id, kMaxDecks> underrunClip_{};         // clip already reported as underrunning, per lane
    bool wasPlaying_ = false;
    bool overloaded_ = false; // the last block was late
    int64_t prerollFrames_ = 0; // left to wait for streamed audio
    double sampleRate_ = 48000.0;
    double position_ = 0.0;
    float transportGain_ = 0.0f; // fades in on play and out on pause, to avoid clicks
    std::vector<int> physicalToBuffer_;
    std::array<std::vector<float>, kMaxDecks> deckLeft_, deckRight_;
    std::vector<float> mixLeft_, mixRight_;
    int maxBlock_ = 0;
};

} // namespace vmp

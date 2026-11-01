#pragma once

#include "engine/Deck.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace vmp
{

enum class OutputMode
{
    InternalMix, // decks summed through the internal mixer to one stereo pair (laptop use)
    DirectOut    // each deck dry to its own stereo pair, mixing done on a hardware mixer
};

// Owns the decks and routes them to device outputs. Knows nothing about JUCE: the host passes a
// map from physical device channel to the index in the buffer array the driver hands us, because
// drivers only hand over the channels that are enabled.
class Engine
{
public:
    static constexpr int kMaxDecks = 4;
    static constexpr int kNoOutput = -1;

    Engine();

    Deck& deck(int index) { return *decks_[static_cast<size_t>(index)]; }
    int numDecks() const { return numDecks_.load(); }
    void setNumDecks(int count);

    // --- Controls (message thread, lock-free) ------------------------------
    void setOutputMode(OutputMode mode) { mode_.store(mode); }
    OutputMode outputMode() const { return mode_.load(); }

    // Outputs are given as the first physical channel of a stereo pair, or kNoOutput.
    void setDeckOutput(int deckIndex, int firstPhysicalChannel);
    int deckOutput(int deckIndex) const;
    void setMasterOutput(int firstPhysicalChannel) { masterOutput_.store(firstPhysicalChannel); }
    int masterOutput() const { return masterOutput_.load(); }

    // Linear gain for the internal mixer's channel fader. Ignored in DirectOut mode.
    void setChannelGain(int deckIndex, float gain);

    // Message thread housekeeping: frees tracks the audio thread has finished with.
    void collectGarbage();

    // --- Non-real-time -----------------------------------------------------
    // physicalToBuffer[ch] is the buffer index for physical output ch, or -1 if disabled.
    void prepare(double sampleRate, int maxBlockFrames, std::vector<int> physicalToBuffer);

    // --- Audio thread ------------------------------------------------------
    void render(float* const* outputs, int numOutputs, int numFrames);

private:
    void renderChunk(float* const* outputs, int numOutputs, int offset, int numFrames);
    float* outputFor(float* const* outputs, int numOutputs, int physicalChannel) const;

    std::array<std::unique_ptr<Deck>, kMaxDecks> decks_;
    std::atomic<int> numDecks_{2};
    std::atomic<OutputMode> mode_{OutputMode::InternalMix};
    std::array<std::atomic<int>, kMaxDecks> deckOutputs_;
    std::atomic<int> masterOutput_{0};
    std::array<std::atomic<float>, kMaxDecks> gains_;

    // Audio thread state.
    std::vector<int> physicalToBuffer_;
    std::array<float, kMaxDecks> appliedGains_{};
    std::array<std::vector<float>, kMaxDecks> deckLeft_, deckRight_;
    std::vector<float> mixLeft_, mixRight_;
    int maxBlock_ = 0;
};

} // namespace vmp

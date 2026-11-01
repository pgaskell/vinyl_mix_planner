#include "engine/Engine.h"

#include <algorithm>

namespace vmp
{

Engine::Engine()
{
    for (size_t i = 0; i < kMaxDecks; ++i)
    {
        decks_[i] = std::make_unique<Deck>();
        deckOutputs_[i].store(static_cast<int>(i) * 2); // deck 1 -> 1/2, deck 2 -> 3/4, ...
        gains_[i].store(1.0f);
        appliedGains_[i] = 1.0f;
    }
}

void Engine::setNumDecks(int count)
{
    numDecks_.store(std::clamp(count, 1, kMaxDecks));
}

void Engine::setDeckOutput(int deckIndex, int firstPhysicalChannel)
{
    if (deckIndex >= 0 && deckIndex < kMaxDecks)
        deckOutputs_[static_cast<size_t>(deckIndex)].store(firstPhysicalChannel);
}

int Engine::deckOutput(int deckIndex) const
{
    if (deckIndex < 0 || deckIndex >= kMaxDecks)
        return kNoOutput;
    return deckOutputs_[static_cast<size_t>(deckIndex)].load();
}

void Engine::setChannelGain(int deckIndex, float gain)
{
    if (deckIndex >= 0 && deckIndex < kMaxDecks)
        gains_[static_cast<size_t>(deckIndex)].store(std::max(0.0f, gain));
}

void Engine::collectGarbage()
{
    for (auto& deck : decks_)
        deck->collectGarbage();
}

void Engine::prepare(double sampleRate, int maxBlockFrames, std::vector<int> physicalToBuffer)
{
    maxBlock_ = std::max(maxBlockFrames, 1);
    physicalToBuffer_ = std::move(physicalToBuffer);
    const auto size = static_cast<size_t>(maxBlock_);
    for (size_t i = 0; i < kMaxDecks; ++i)
    {
        decks_[i]->prepare(sampleRate, maxBlock_);
        deckLeft_[i].assign(size, 0.0f);
        deckRight_[i].assign(size, 0.0f);
    }
    mixLeft_.assign(size, 0.0f);
    mixRight_.assign(size, 0.0f);
}

float* Engine::outputFor(float* const* outputs, int numOutputs, int physicalChannel) const
{
    if (physicalChannel < 0 || physicalChannel >= static_cast<int>(physicalToBuffer_.size()))
        return nullptr;
    const int index = physicalToBuffer_[static_cast<size_t>(physicalChannel)];
    if (index < 0 || index >= numOutputs)
        return nullptr;
    return outputs[index];
}

void Engine::render(float* const* outputs, int numOutputs, int numFrames)
{
    for (int ch = 0; ch < numOutputs; ++ch)
        if (outputs[ch] != nullptr)
            std::fill_n(outputs[ch], numFrames, 0.0f);

    if (maxBlock_ == 0)
        return;

    for (int done = 0; done < numFrames;)
    {
        const int chunk = std::min(maxBlock_, numFrames - done);
        renderChunk(outputs, numOutputs, done, chunk);
        done += chunk;
    }
}

namespace
{
void addInto(float* dest, const float* src, int numFrames)
{
    if (dest != nullptr)
        for (int i = 0; i < numFrames; ++i)
            dest[i] += src[i];
}
} // namespace

void Engine::renderChunk(float* const* outputs, int numOutputs, int offset, int numFrames)
{
    const auto count = static_cast<size_t>(numDecks_.load());
    for (size_t d = 0; d < count; ++d)
        decks_[d]->render(deckLeft_[d].data(), deckRight_[d].data(), numFrames);

    auto out = [&](int physical) {
        float* buffer = outputFor(outputs, numOutputs, physical);
        return buffer != nullptr ? buffer + offset : nullptr;
    };

    if (mode_.load() == OutputMode::DirectOut)
    {
        // Dry: the hardware mixer does the mixing. Decks sharing a pair are summed.
        for (size_t d = 0; d < count; ++d)
        {
            const int first = deckOutputs_[d].load();
            if (first == kNoOutput)
                continue;
            addInto(out(first), deckLeft_[d].data(), numFrames);
            addInto(out(first + 1), deckRight_[d].data(), numFrames);
        }
        return;
    }

    std::fill_n(mixLeft_.begin(), numFrames, 0.0f);
    std::fill_n(mixRight_.begin(), numFrames, 0.0f);
    for (size_t d = 0; d < count; ++d)
    {
        // Ramp gain across the block so fader moves don't click.
        const float target = gains_[d].load();
        const float step = (target - appliedGains_[d]) / static_cast<float>(numFrames);
        float gain = appliedGains_[d];
        for (int i = 0; i < numFrames; ++i)
        {
            gain += step;
            mixLeft_[static_cast<size_t>(i)] += gain * deckLeft_[d][static_cast<size_t>(i)];
            mixRight_[static_cast<size_t>(i)] += gain * deckRight_[d][static_cast<size_t>(i)];
        }
        appliedGains_[d] = target;
    }

    const int master = masterOutput_.load();
    if (master != kNoOutput)
    {
        addInto(out(master), mixLeft_.data(), numFrames);
        addInto(out(master + 1), mixRight_.data(), numFrames);
    }
}

} // namespace vmp

#include "engine/Engine.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vmp
{

namespace
{
constexpr double kNoSeek = std::numeric_limits<double>::quiet_NaN();

void addInto(float* dest, const float* src, int numFrames)
{
    if (dest != nullptr)
        for (int i = 0; i < numFrames; ++i)
            dest[i] += src[i];
}
} // namespace

Engine::Engine() : seekRequest_(kNoSeek)
{
    for (size_t i = 0; i < kMaxDecks; ++i)
    {
        decks_[i] = std::make_unique<Deck>();
        deckOutputs_[i].store(static_cast<int>(i) * 2); // deck 1 -> 1/2, deck 2 -> 3/4, ...
    }
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

void Engine::prepare(double sampleRate, int maxBlockFrames, std::vector<int> physicalToBuffer)
{
    sampleRate_ = sampleRate;
    maxBlock_ = std::max(maxBlockFrames, 1);
    physicalToBuffer_ = std::move(physicalToBuffer);
    const auto size = static_cast<size_t>(maxBlock_);
    for (size_t i = 0; i < kMaxDecks; ++i)
    {
        decks_[i]->prepare(sampleRate, maxBlock_);
        channels_[i].prepare(sampleRate);
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

    const Schedule* schedule = schedule_.acquire();

    const double seekTo = seekRequest_.exchange(kNoSeek);
    if (!std::isnan(seekTo))
        position_ = std::max(0.0, seekTo);

    const bool playing = playing_.load();
    if (maxBlock_ > 0 && (playing || transportGain_ > 0.0f))
    {
        // Devices occasionally deliver blocks larger than they announced.
        for (int done = 0; done < numFrames;)
        {
            const int chunk = std::min(maxBlock_, numFrames - done);
            renderChunk(schedule, playing, outputs, numOutputs, done, chunk);
            done += chunk;
        }
    }
    playhead_.store(position_);
}

void Engine::renderLane(const std::vector<ScheduledClip>& lane, size_t deck, double start, int numFrames)
{
    float* left = deckLeft_[deck].data();
    float* right = deckRight_[deck].data();
    Deck& player = *decks_[deck];

    for (int done = 0; done < numFrames;)
    {
        const double t = start + done / sampleRate_;
        // The last clip to start at or before t is the one playing (on overlap, the newest wins).
        auto next = std::upper_bound(lane.begin(), lane.end(), t,
                                     [](double time, const ScheduledClip& c) { return time < c.start; });
        const ScheduledClip* active = next != lane.begin() && t < std::prev(next)->end ? &*std::prev(next) : nullptr;

        double segmentEnd = next != lane.end() ? next->start : std::numeric_limits<double>::infinity();
        if (active != nullptr)
            segmentEnd = std::min(segmentEnd, active->end);
        int frames = numFrames - done;
        if (std::isfinite(segmentEnd))
            frames = std::clamp(static_cast<int>(std::ceil((segmentEnd - t) * sampleRate_)), 1, frames);

        if (active != nullptr)
        {
            player.render(active->audio.get(), active->timing.sourceFrameAt(t),
                          active->timing.sourceFrameAt(t + frames / sampleRate_), left + done, right + done, frames);
            laneMixer_[deck] = active->mixer;
        }
        else
        {
            player.renderSilence(left + done, right + done, frames);
        }
        done += frames;
    }
}

void Engine::renderChunk(const Schedule* schedule, bool playing, float* const* outputs, int numOutputs, int offset,
                         int numFrames)
{
    const auto count = schedule != nullptr ? static_cast<size_t>(schedule->numDecks) : 0;
    const bool direct = mode_.load() == OutputMode::DirectOut;

    for (size_t d = 0; d < count; ++d)
    {
        renderLane(schedule->lanes[d], d, position_, numFrames);
        if (!direct)
            channels_[d].process(laneMixer_[d], deckLeft_[d].data(), deckRight_[d].data(), numFrames);
    }

    auto out = [&](int physical) {
        float* buffer = outputFor(outputs, numOutputs, physical);
        return buffer != nullptr ? buffer + offset : nullptr;
    };

    if (direct)
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
    }
    else
    {
        std::fill_n(mixLeft_.begin(), numFrames, 0.0f);
        std::fill_n(mixRight_.begin(), numFrames, 0.0f);
        for (size_t d = 0; d < count; ++d)
            for (size_t i = 0; i < static_cast<size_t>(numFrames); ++i)
            {
                mixLeft_[i] += deckLeft_[d][i];
                mixRight_[i] += deckRight_[d][i];
            }
        const int master = masterOutput_.load();
        if (master != kNoOutput)
        {
            addInto(out(master), mixLeft_.data(), numFrames);
            addInto(out(master + 1), mixRight_.data(), numFrames);
        }
    }

    // Transport fade, applied to everything written in this chunk.
    const float target = playing ? 1.0f : 0.0f;
    if (transportGain_ != 1.0f || target != 1.0f)
    {
        const float step = (target - transportGain_) / static_cast<float>(numFrames);
        for (int ch = 0; ch < numOutputs; ++ch)
        {
            if (outputs[ch] == nullptr)
                continue;
            float gain = transportGain_;
            for (int i = 0; i < numFrames; ++i)
            {
                gain += step;
                outputs[ch][offset + i] *= gain;
            }
        }
    }
    transportGain_ = target;
    position_ += numFrames / sampleRate_;
}

} // namespace vmp

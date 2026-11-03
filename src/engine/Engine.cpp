#include "engine/Engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace vmp
{

namespace
{
constexpr double kNoSeek = std::numeric_limits<double>::quiet_NaN();

int64_t steadyNanoseconds()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Raises each side's peak to its loudest sample. Only the audio thread raises them, so a plain
// load and store is enough; the reader resets them with exchange().
void notePeak(std::array<std::atomic<float>, 2>& peak, const float* left, const float* right, int numFrames)
{
    float loudest[2] = {0.0f, 0.0f};
    for (int i = 0; i < numFrames; ++i)
    {
        loudest[0] = std::max(loudest[0], std::abs(left[i]));
        loudest[1] = std::max(loudest[1], std::abs(right[i]));
    }
    for (size_t side = 0; side < 2; ++side)
        if (loudest[side] > peak[side].load(std::memory_order_relaxed))
            peak[side].store(loudest[side], std::memory_order_relaxed);
}

void addInto(float* dest, const float* src, int numFrames)
{
    if (dest != nullptr)
        for (int i = 0; i < numFrames; ++i)
            dest[i] += src[i];
}
} // namespace

Engine::Engine() : seekRequest_(kNoSeek), clock_(steadyNanoseconds)
{
    for (size_t i = 0; i < kMaxDecks; ++i)
    {
        decks_[i] = std::make_unique<Deck>();
        deckOutputs_[i].store(static_cast<int>(i) * 2); // deck 1 -> 1/2, deck 2 -> 3/4, ...
        for (auto& control : liveControls_[i])
            control.store(std::numeric_limits<float>::quiet_NaN());
        for (auto& scale : liveScales_[i])
            scale.store(std::numeric_limits<float>::quiet_NaN());
    }
}

static_assert(static_cast<int>(Param::Color) - static_cast<int>(Param::Volume) == 4, "liveControls_ order");

void Engine::setLiveControl(int deck, Param param, std::optional<double> position)
{
    if (deck < 0 || deck >= kMaxDecks || param == Param::Pitch)
        return;
    const auto index = static_cast<size_t>(param) - static_cast<size_t>(Param::Volume);
    liveControls_[static_cast<size_t>(deck)][index].store(
        position ? static_cast<float>(std::clamp(*position, 0.0, 1.0)) : std::numeric_limits<float>::quiet_NaN());
}

void Engine::setLiveTrim(int deck, std::optional<double> position)
{
    if (deck < 0 || deck >= kMaxDecks)
        return;
    liveControls_[static_cast<size_t>(deck)][kLiveTrim].store(
        position ? static_cast<float>(std::clamp(*position, 0.0, 1.0)) : std::numeric_limits<float>::quiet_NaN());
}

void Engine::setLiveScale(int deck, Param param, std::optional<double> amount)
{
    if (deck < 0 || deck >= kMaxDecks || param == Param::Pitch)
        return;
    const auto index = static_cast<size_t>(param) - static_cast<size_t>(Param::Volume);
    liveScales_[static_cast<size_t>(deck)][index].store(amount ? static_cast<float>(*amount)
                                                               : std::numeric_limits<float>::quiet_NaN());
}

ChannelSettings Engine::withLiveControls(ChannelSettings settings, size_t deck) const
{
    auto take = [&](double& field, size_t index) {
        const float live = liveControls_[deck][index].load(std::memory_order_relaxed);
        const float scale = liveScales_[deck][index].load(std::memory_order_relaxed);
        if (!std::isnan(live))
            field = live;
        else if (!std::isnan(scale))
            field = MixerChannel::trimmed(static_cast<Param>(static_cast<size_t>(Param::Volume) + index), field, scale);
    };
    take(settings.fader, 0);
    take(settings.high, 1);
    take(settings.mid, 2);
    take(settings.low, 3);
    take(settings.color, 4);
    take(settings.trim, kLiveTrim);
    return settings;
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

double Engine::prefetchTarget() const
{
    const double seek = seekRequest_.load();
    return std::isnan(seek) ? playhead_.load() : seek;
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

namespace
{
// Bumps the render counter on entry and exit (see Prefetcher::release for the pairing fence).
struct RenderScope
{
    explicit RenderScope(std::atomic<uint64_t>& counter) : counter_(counter)
    {
        counter_.fetch_add(1);
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }
    ~RenderScope() { counter_.fetch_add(1); }
    std::atomic<uint64_t>& counter_;
};
} // namespace

bool Engine::audioLoaded(const Schedule* schedule, double start, int numFrames) const
{
    if (schedule == nullptr)
        return true;
    const double end = start + numFrames / sampleRate_;
    for (int d = 0; d < schedule->numDecks; ++d)
        for (const auto& clip : schedule->lanes[static_cast<size_t>(d)])
        {
            if (clip.audio == nullptr || clip.end <= start || clip.start >= end)
                continue;
            const double margin = Deck::kReadMarginSeconds * clip.audio->sampleRate;
            const double from = clip.timing.sourceFrameAt(std::max(start, clip.start)) - margin;
            const double to = clip.timing.sourceFrameAt(std::min(end, clip.end)) + margin;
            if (!clip.audio->resident(static_cast<int64_t>(std::floor(from)), static_cast<int64_t>(std::ceil(to))))
                return false;
        }
    return true;
}

void Engine::render(float* const* outputs, int numOutputs, int numFrames)
{
    const RenderScope scope(renderCounter_);
    const int64_t began = clock_();

    for (int ch = 0; ch < numOutputs; ++ch)
        if (outputs[ch] != nullptr)
            std::fill_n(outputs[ch], numFrames, 0.0f);

    const Schedule* schedule = schedule_.acquire();

    const double seekTo = seekRequest_.exchange(kNoSeek);
    if (!std::isnan(seekTo))
    {
        position_ = std::max(0.0, seekTo);
        silentClip_.fill(0); // report missing or late audio again
        underrunClip_.fill(0);
        prerollFrames_ = static_cast<int64_t>(kPrerollSeconds * sampleRate_);
    }

    const bool playing = playing_.load();
    if (playing && !wasPlaying_)
        prerollFrames_ = static_cast<int64_t>(kPrerollSeconds * sampleRate_);
    wasPlaying_ = playing;

    // Starting (or jumping) somewhere whose audio isn't loaded yet: hold the playhead in silence
    // for a moment instead of playing gaps. Decided before any deck moves.
    if (playing && prerollFrames_ > 0)
    {
        if (!audioLoaded(schedule, position_, numFrames))
        {
            prerollFrames_ -= numFrames;
            transportGain_ = 0.0f; // fade in once it's ready
            playhead_.store(position_);
            return;
        }
        prerollFrames_ = 0;
    }

    const bool audible = maxBlock_ > 0 && (playing || transportGain_ > 0.0f);
    if (!audible)
    {
        silentClip_.fill(0); // report missing or late audio again next time
        underrunClip_.fill(0);
    }
    else
    {
        const double startedAt = position_;
        // Devices occasionally deliver blocks larger than they announced.
        for (int done = 0; done < numFrames;)
        {
            const int chunk = std::min(maxBlock_, numFrames - done);
            renderChunk(schedule, playing, outputs, numOutputs, done, chunk);
            done += chunk;
        }

        // Missing the deadline means the driver had nothing to play: a dropout. Only counted while
        // something is audible; reported where the block began.
        const double budget = numFrames / sampleRate_;
        const double used = static_cast<double>(clock_() - began) * 1e-9;
        const auto load = static_cast<float>(used / budget);
        if (load > peakLoad_.load(std::memory_order_relaxed))
            peakLoad_.store(load, std::memory_order_relaxed);
        // A run of late blocks is one glitch, reported at its first block.
        if (used > budget && !overloaded_)
            glitches_.push({GlitchKind::Overload, -1, startedAt, static_cast<float>(used * 1e3),
                            static_cast<float>(budget * 1e3)});
        overloaded_ = used > budget;
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

        if (active != nullptr && active->audio == nullptr)
        {
            if (silentClip_[deck] != active->clip.id)
            {
                silentClip_[deck] = active->clip.id;
                glitches_.push({GlitchKind::NotLoaded, static_cast<int>(deck), t});
            }
            player.renderSilence(left + done, right + done, frames);
        }
        else if (active != nullptr &&
                 !active->audio->resident(
                     static_cast<int64_t>(std::floor(active->timing.sourceFrameAt(t) -
                                                     Deck::kReadMarginSeconds * active->audio->sampleRate)),
                     static_cast<int64_t>(std::ceil(active->timing.sourceFrameAt(t + frames / sampleRate_) +
                                                    Deck::kReadMarginSeconds * active->audio->sampleRate))))
        {
            // Streamed audio that didn't arrive in time.
            if (underrunClip_[deck] != active->clip.id)
            {
                underrunClip_[deck] = active->clip.id;
                glitches_.push({GlitchKind::Underrun, static_cast<int>(deck), t});
            }
            player.renderSilence(left + done, right + done, frames);
        }
        else if (active != nullptr)
        {
            underrunClip_[deck] = 0;
            player.render(active->audio.get(), active->timing.sourceFrameAt(t),
                          active->timing.sourceFrameAt(t + frames / sampleRate_), left + done, right + done, frames);
            // Automation is read once per segment; the mixer channel glides between blocks.
            laneMixer_[deck] = mixerAt(active->clip, active->timing.sourceFrameAt(t));
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
            channels_[d].process(withLiveControls(laneMixer_[d], d), deckLeft_[d].data(), deckRight_[d].data(), numFrames);
        notePeak(deckPeaks_[d], deckLeft_[d].data(), deckRight_[d].data(), numFrames);
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
        notePeak(masterPeak_, mixLeft_.data(), mixRight_.data(), numFrames);
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

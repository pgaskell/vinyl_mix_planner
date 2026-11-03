#include "engine/Bounce.h"

#include "engine/Engine.h"
#include "engine/Prefetcher.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace vmp
{

namespace
{
constexpr int kBlockFrames = 1024;

// A copy of the schedule whose streamed tracks are opened again, so this bounce's prefetcher loads
// and unloads chunks without touching the ones the live engine is playing. Null if any clip has
// no audio.
std::unique_ptr<Schedule> ownCopy(const Schedule& schedule)
{
    auto copy = std::make_unique<Schedule>(schedule);
    std::map<const TrackAudio*, std::shared_ptr<const TrackAudio>> reopened;
    for (auto& lane : copy->lanes)
        for (auto& clip : lane)
        {
            if (clip.audio == nullptr)
                return nullptr;
            if (clip.audio->stream == nullptr)
                continue; // in memory: read-only, safe to share
            auto& audio = reopened[clip.audio.get()];
            if (audio == nullptr)
            {
                auto track = std::make_shared<TrackAudio>();
                track->frames = clip.audio->frames;
                track->sampleRate = clip.audio->sampleRate;
                track->stream = clip.audio->stream->openAgain();
                if (track->stream == nullptr)
                    return nullptr;
                audio = std::move(track);
            }
            clip.audio = audio;
        }
    return copy;
}
} // namespace

std::optional<BounceRange> bounceRange(const Schedule& schedule)
{
    std::optional<BounceRange> range;
    for (int d = 0; d < schedule.numDecks; ++d)
        for (const auto& clip : schedule.lanes[static_cast<size_t>(d)])
            range = range ? BounceRange{std::min(range->start, clip.start), std::max(range->end, clip.end)}
                          : BounceRange{clip.start, clip.end};
    return range;
}

BounceResult bounce(const Schedule& schedule, double sampleRate, BounceRange range,
                    const std::function<bool(const float* const* channels, int numFrames)>& write,
                    const std::function<bool(double fraction)>& progress)
{
    BounceResult result;
    auto own = ownCopy(schedule);
    if (own == nullptr)
    {
        result.status = BounceResult::Status::MissingAudio;
        return result;
    }

    Engine engine;
    engine.prepare(sampleRate, kBlockFrames, {0, 1});
    engine.setOutputMode(OutputMode::InternalMix);
    engine.setMasterOutput(0);
    engine.setClock([]() -> int64_t { return 0; }); // offline: there's no deadline to miss
    engine.seek(range.start);
    engine.playWithoutFade(); // the first needle drop keeps its attack
    Prefetcher prefetcher(engine);
    prefetcher.setSchedule(std::move(own));

    const auto total = static_cast<int64_t>(std::llround(std::max(0.0, range.end - range.start) * sampleRate));
    std::vector<float> left(kBlockFrames), right(kBlockFrames);
    float* channels[2] = {left.data(), right.data()};
    const auto progressEvery = static_cast<int64_t>(sampleRate / 4.0);
    int64_t nextProgress = 0;
    while (result.frames < total)
    {
        if (result.frames >= nextProgress)
        {
            nextProgress += progressEvery;
            if (progress && !progress(static_cast<double>(result.frames) / static_cast<double>(total)))
            {
                result.status = BounceResult::Status::Cancelled;
                return result;
            }
        }
        // Synchronous here: whatever the next few seconds need is loaded before they're rendered.
        prefetcher.update();
        const int n = static_cast<int>(std::min<int64_t>(kBlockFrames, total - result.frames));
        engine.render(channels, 2, n);
        for (const float* data : channels)
            for (int i = 0; i < n; ++i)
            {
                const float magnitude = std::abs(data[i]);
                result.peak = std::max(result.peak, magnitude);
                result.clippedSamples += magnitude > 1.0f ? 1 : 0;
            }
        if (!write(channels, n))
        {
            result.status = BounceResult::Status::WriteFailed;
            return result;
        }
        result.frames += n;
    }

    for (Glitch glitch; engine.popGlitch(glitch);)
        if (glitch.kind != GlitchKind::Overload)
            result.glitches.push_back(glitch);
    if (!result.glitches.empty() || engine.takeDroppedGlitches() > 0)
        result.status = BounceResult::Status::Glitched;
    if (progress)
        progress(1.0);
    return result;
}

} // namespace vmp

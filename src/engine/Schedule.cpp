#include "engine/Schedule.h"

#include <algorithm>
#include <stdexcept>

namespace vmp
{

std::unique_ptr<Schedule> buildSchedule(const Set& set, const AudioLookup& lookup)
{
    auto schedule = std::make_unique<Schedule>();
    schedule->numDecks = std::clamp(set.numDecks, 1, kMaxDecks);

    for (const auto& clip : set.clips)
    {
        const Track* track = set.findTrack(clip.trackId);
        if (track == nullptr || clip.deck < 0 || clip.deck >= schedule->numDecks)
            continue;
        auto audio = lookup(*track);
        if (audio == nullptr)
            continue;
        try
        {
            ClipTiming timing(clip, *track);
            const double start = timing.startSeconds(), end = timing.endSeconds();
            schedule->lanes[static_cast<size_t>(clip.deck)].push_back(
                {clip.id, start, end, std::move(timing), std::move(audio), clip.mixer});
        }
        catch (const std::invalid_argument&)
        {
        }
    }

    for (auto& lane : schedule->lanes)
        std::stable_sort(lane.begin(), lane.end(),
                         [](const ScheduledClip& a, const ScheduledClip& b) { return a.start < b.start; });
    return schedule;
}

} // namespace vmp

#include "core/Edits.h"

#include "core/Pitch.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace vmp::edit
{

namespace
{
Clip* findClip(Set& set, Id id)
{
    auto it = std::find_if(set.clips.begin(), set.clips.end(), [id](const Clip& c) { return c.id == id; });
    return it == set.clips.end() ? nullptr : &*it;
}

std::optional<ClipTiming> timingFor(const Clip& clip, const Track& track)
{
    try
    {
        return ClipTiming(clip, track);
    }
    catch (const std::invalid_argument&)
    {
        return std::nullopt;
    }
}

bool deckInRange(const Set& set, int deck)
{
    return deck >= 0 && deck < set.numDecks;
}

double clamp01(double v)
{
    return std::clamp(v, 0.0, 1.0);
}
} // namespace

Id addTrack(Set& set, Track track)
{
    for (const auto& existing : set.tracks)
        if (existing.path == track.path)
            return existing.id;
    track.id = set.allocateId();
    set.tracks.push_back(std::move(track));
    return set.tracks.back().id;
}

Id addClip(Set& set, Id trackId, int deck, double startSeconds)
{
    const Track* track = set.findTrack(trackId);
    if (track == nullptr || !deckInRange(set, deck) || !std::isfinite(startSeconds) || track->frames <= 0)
        return 0;

    Clip clip;
    clip.id = set.allocateId();
    clip.trackId = trackId;
    clip.deck = deck;
    clip.startSeconds = startSeconds;
    clip.inFrame = 0;
    clip.outFrame = track->frames;
    set.clips.push_back(clip);
    return clip.id;
}

bool deleteClip(Set& set, Id clipId)
{
    auto it = std::find_if(set.clips.begin(), set.clips.end(), [clipId](const Clip& c) { return c.id == clipId; });
    if (it == set.clips.end())
        return false;
    const Id trackId = it->trackId;
    set.clips.erase(it);

    const bool trackUsed =
        std::any_of(set.clips.begin(), set.clips.end(), [trackId](const Clip& c) { return c.trackId == trackId; });
    if (!trackUsed)
        std::erase_if(set.tracks, [trackId](const Track& t) { return t.id == trackId; });
    return true;
}

bool moveClip(Set& set, Id clipId, double startSeconds, int deck)
{
    Clip* clip = findClip(set, clipId);
    if (clip == nullptr || !deckInRange(set, deck) || !std::isfinite(startSeconds))
        return false;
    clip->startSeconds = startSeconds;
    clip->deck = deck;
    return true;
}

bool trimClipStart(Set& set, Id clipId, double newStartSeconds)
{
    Clip* clip = findClip(set, clipId);
    const Track* track = clip != nullptr ? set.findTrack(clip->trackId) : nullptr;
    if (track == nullptr || !std::isfinite(newStartSeconds))
        return false;
    const auto timing = timingFor(*clip, *track);
    if (!timing)
        return false;

    const double earliest = timing->timelineSecondsAt(0.0);
    double latest = timing->endSeconds() - kMinClipSeconds;
    for (const auto& move : clip->pitchMoves)
        if (move.sourceFrame >= clip->inFrame)
            latest = std::min(latest, timing->timelineSecondsAt(static_cast<double>(move.sourceFrame)));
    if (latest < earliest)
        return false;

    const double target = std::clamp(newStartSeconds, earliest, latest);
    const auto newIn = std::clamp<int64_t>(std::llround(timing->sourceFrameAt(target)), 0, clip->outFrame - 1);
    // Recompute the start from the whole-frame in-point so the music doesn't shift by the rounding.
    clip->startSeconds = timing->timelineSecondsAt(static_cast<double>(newIn));
    clip->inFrame = newIn;
    return true;
}

bool trimClipEnd(Set& set, Id clipId, double newEndSeconds)
{
    Clip* clip = findClip(set, clipId);
    const Track* track = clip != nullptr ? set.findTrack(clip->trackId) : nullptr;
    if (track == nullptr || !std::isfinite(newEndSeconds))
        return false;
    const auto timing = timingFor(*clip, *track);
    if (!timing)
        return false;

    const double earliest = clip->startSeconds + kMinClipSeconds;
    const double latest = timing->timelineSecondsAt(static_cast<double>(track->frames));
    const double target = std::clamp(newEndSeconds, earliest, std::max(earliest, latest));
    clip->outFrame = std::clamp<int64_t>(std::llround(timing->sourceFrameAt(target)), clip->inFrame + 1, track->frames);
    return true;
}

bool setClipPitch(Set& set, Id clipId, int pitchSteps, std::optional<double> pivotSeconds)
{
    Clip* clip = findClip(set, clipId);
    const Track* track = clip != nullptr ? set.findTrack(clip->trackId) : nullptr;
    if (track == nullptr)
        return false;
    const auto before = timingFor(*clip, *track);
    if (!before)
        return false;

    Clip changed = *clip;
    changed.pitchSteps = clampPitchSteps(pitchSteps);
    // A slower start can make an existing ramp run into the next move.
    const auto after = timingFor(changed, *track);
    if (!after)
        return false;

    if (pivotSeconds && *pivotSeconds > before->startSeconds() && *pivotSeconds < before->endSeconds())
    {
        const double frame = before->sourceFrameAt(*pivotSeconds);
        changed.startSeconds += *pivotSeconds - after->timelineSecondsAt(frame);
    }
    *clip = changed;
    return true;
}

bool setClipMixer(Set& set, Id clipId, const ChannelSettings& settings)
{
    Clip* clip = findClip(set, clipId);
    if (clip == nullptr)
        return false;
    clip->mixer = {clamp01(settings.fader), clamp01(settings.high), clamp01(settings.mid), clamp01(settings.low)};
    return true;
}

bool setTrackGrid(Set& set, Id trackId, std::optional<BeatGrid> grid)
{
    auto it = std::find_if(set.tracks.begin(), set.tracks.end(), [trackId](const Track& t) { return t.id == trackId; });
    if (it == set.tracks.end())
        return false;
    if (grid && (!std::isfinite(grid->bpm) || grid->bpm < 20.0 || grid->bpm > 400.0 ||
                 !std::isfinite(grid->firstBeatSeconds)))
        return false;
    if (grid) // keep the stored offset small: any beat of the grid will do
        grid->firstBeatSeconds -= std::floor(grid->firstBeatSeconds / (4.0 * grid->periodSeconds())) *
                                  4.0 * grid->periodSeconds();
    it->grid = grid;
    return true;
}

bool setTrackKey(Set& set, Id trackId, std::optional<MusicalKey> key)
{
    auto it = std::find_if(set.tracks.begin(), set.tracks.end(), [trackId](const Track& t) { return t.id == trackId; });
    if (it == set.tracks.end())
        return false;
    it->key = key;
    return true;
}

bool setNumDecks(Set& set, int numDecks)
{
    if (numDecks < 1 || numDecks > kMaxDecks)
        return false;
    if (std::any_of(set.clips.begin(), set.clips.end(), [numDecks](const Clip& c) { return c.deck >= numDecks; }))
        return false;
    set.numDecks = numDecks;
    return true;
}

} // namespace vmp::edit

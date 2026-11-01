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

// After an edit that changes the clip's speed, slides the clip so the music that was at
// `pivotSeconds` is there again, as when you move the pitch fader on a playing deck. Does nothing
// if the pivot is outside the clip.
void keepMusicAt(Clip& clip, const Track& track, const ClipTiming& before, std::optional<double> pivotSeconds)
{
    if (!pivotSeconds || *pivotSeconds <= before.startSeconds() || *pivotSeconds >= before.endSeconds())
        return;
    const double frame = before.sourceFrameAt(*pivotSeconds);
    clip.startSeconds += *pivotSeconds - ClipTiming(clip, track).timelineSecondsAt(frame);
}

double clampValue(Param param, double value)
{
    const auto range = paramRange(param);
    if (param == Param::Pitch)
        value = std::round(value); // whole 0.05% steps
    return std::clamp(value, range.min, range.max);
}

// The starting value for a mixer control. Not for pitch.
double& mixerValue(ChannelSettings& m, Param param)
{
    switch (param)
    {
    case Param::High:
        return m.high;
    case Param::Mid:
        return m.mid;
    case Param::Low:
        return m.low;
    case Param::Volume:
    case Param::Pitch:
        break;
    }
    return m.fader;
}

void sortPoints(std::vector<AutomationPoint>& points)
{
    std::stable_sort(points.begin(), points.end(),
                     [](const AutomationPoint& a, const AutomationPoint& b) { return a.sourceFrame < b.sourceFrame; });
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
    const double latest = timing->endSeconds() - kMinClipSeconds;
    if (latest < earliest)
        return false;

    const double target = std::clamp(newStartSeconds, earliest, latest);
    const auto newIn = std::clamp<int64_t>(std::llround(timing->sourceFrameAt(target)), 0, clip->outFrame - 1);
    // Recompute the start from the whole-frame in-point so the music doesn't shift by the rounding.
    const double newStart = timing->timelineSecondsAt(static_cast<double>(newIn));

    // Automated controls start where their automation was at the new needle drop, so the clip
    // sounds the same from there on. Pitch uses the exact speed, which may fall between steps.
    const Clip old = *clip;
    for (int p = 0; p < kNumParams; ++p)
    {
        const auto param = static_cast<Param>(p);
        if (old.points(param).empty())
            continue;
        if (param == Param::Pitch)
            clip->pitchSteps = pitchStepsFromSpeed(timing->speedAt(newStart));
        else
            mixerValue(clip->mixer, param) = valueAt(old, param, static_cast<double>(newIn));
    }
    clip->startSeconds = newStart;
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
    const ClipTiming before(*clip, *track);
    clip->pitchSteps = clampPitchSteps(pitchSteps);
    keepMusicAt(*clip, *track, before, pivotSeconds);
    return true;
}

bool setAutomationPoint(Set& set, Id clipId, Param param, int64_t sourceFrame, double value)
{
    Clip* clip = findClip(set, clipId);
    const Track* track = clip != nullptr ? set.findTrack(clip->trackId) : nullptr;
    if (track == nullptr || sourceFrame < 0 || sourceFrame > track->frames || !std::isfinite(value))
        return false;
    auto& points = clip->points(param);
    const AutomationPoint point{sourceFrame, clampValue(param, value)};
    auto same = std::find_if(points.begin(), points.end(), [&](const auto& p) { return p.sourceFrame == sourceFrame; });
    if (same != points.end())
        *same = point;
    else
        points.push_back(point);
    sortPoints(points);
    return true;
}

bool moveAutomationPoint(Set& set, Id clipId, Param param, size_t index, int64_t sourceFrame, double value)
{
    Clip* clip = findClip(set, clipId);
    const Track* track = clip != nullptr ? set.findTrack(clip->trackId) : nullptr;
    if (track == nullptr || index >= clip->points(param).size() || !std::isfinite(value))
        return false;
    auto& points = clip->points(param);
    // Stays between its neighbours so the order (and the caller's index) holds.
    const int64_t low = index > 0 ? points[index - 1].sourceFrame : 0;
    const int64_t high = index + 1 < points.size() ? points[index + 1].sourceFrame : track->frames;
    points[index] = {std::clamp(sourceFrame, low, high), clampValue(param, value)};
    return true;
}

bool deleteAutomationPoint(Set& set, Id clipId, Param param, size_t index)
{
    Clip* clip = findClip(set, clipId);
    if (clip == nullptr || index >= clip->points(param).size())
        return false;
    auto& points = clip->points(param);
    points.erase(points.begin() + static_cast<long>(index));
    return true;
}

bool clearAutomation(Set& set, Id clipId, Param param)
{
    Clip* clip = findClip(set, clipId);
    if (clip == nullptr || clip->points(param).empty())
        return false;
    // Keep the sound at the start of the clip: its first value becomes the starting value.
    const double first = valueAt(*clip, param, static_cast<double>(clip->inFrame));
    clip->points(param).clear();
    if (param == Param::Pitch)
        clip->pitchSteps = std::clamp(first, -double(kMaxPitchSteps), double(kMaxPitchSteps));
    else
        mixerValue(clip->mixer, param) = clampValue(param, first);
    return true;
}

bool setStartValue(Set& set, Id clipId, Param param, double value)
{
    Clip* clip = findClip(set, clipId);
    if (clip == nullptr || !std::isfinite(value))
        return false;
    if (param == Param::Pitch)
        clip->pitchSteps = clampValue(param, value);
    else
        mixerValue(clip->mixer, param) = clampValue(param, value);
    return true;
}

bool setParamAt(Set& set, Id clipId, Param param, double value, double playheadSeconds)
{
    Clip* clip = findClip(set, clipId);
    const Track* track = clip != nullptr ? set.findTrack(clip->trackId) : nullptr;
    if (track == nullptr || !std::isfinite(value))
        return false;

    if (clip->points(param).empty())
    {
        if (param == Param::Pitch)
            return setClipPitch(set, clipId, static_cast<int>(std::lround(value)), playheadSeconds);
        mixerValue(clip->mixer, param) = clampValue(param, value);
        return true;
    }

    // Automated: write a keypoint at the music under the playhead, replacing one close by. At the
    // needle drop itself, that's the starting point.
    const ClipTiming before(*clip, *track);
    if (playheadSeconds < before.startSeconds() || playheadSeconds > before.endSeconds())
        return false;
    if (playheadSeconds - before.startSeconds() <= kKeypointSnapSeconds)
        return setStartValue(set, clipId, param, value);
    const auto frame = static_cast<int64_t>(std::llround(before.sourceFrameAt(playheadSeconds)));
    const auto nearby = static_cast<int64_t>(kKeypointSnapSeconds * track->sampleRate);
    auto& points = clip->points(param);
    auto close = std::find_if(points.begin(), points.end(),
                              [&](const auto& p) { return std::llabs(p.sourceFrame - frame) <= nearby; });
    if (close != points.end())
        close->value = clampValue(param, value);
    else
    {
        points.push_back({frame, clampValue(param, value)});
        sortPoints(points);
    }
    if (param == Param::Pitch)
        keepMusicAt(*clip, *track, before, playheadSeconds);
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

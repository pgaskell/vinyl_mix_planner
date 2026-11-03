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
        value = std::round(value); // whole steps
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
    case Param::Color:
        return m.color;
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

std::vector<RecordedMove> thinMoves(const std::vector<RecordedMove>& moves, double tolerance)
{
    if (moves.size() <= 2)
        return moves;
    std::vector<bool> keep(moves.size(), false);
    keep.front() = keep.back() = true;
    std::vector<std::pair<size_t, size_t>> spans{{0, moves.size() - 1}};
    while (!spans.empty())
    {
        const auto [a, b] = spans.back();
        spans.pop_back();
        const double span = moves[b].seconds - moves[a].seconds;
        double worst = 0.0;
        size_t at = a;
        for (size_t i = a + 1; i < b; ++i)
        {
            const double t = span > 0.0 ? (moves[i].seconds - moves[a].seconds) / span : 0.0;
            const double line = moves[a].value + t * (moves[b].value - moves[a].value);
            if (const double error = std::abs(moves[i].value - line); error > worst)
            {
                worst = error;
                at = i;
            }
        }
        if (worst > tolerance)
        {
            keep[at] = true;
            spans.push_back({a, at});
            spans.push_back({at, b});
        }
    }
    std::vector<RecordedMove> kept;
    for (size_t i = 0; i < moves.size(); ++i)
        if (keep[i])
            kept.push_back(moves[i]);
    return kept;
}

bool writeRecordedPass(Set& set, Id clipId, Param param, const std::vector<RecordedMove>& moves, double endSeconds)
{
    Clip* clip = findClip(set, clipId);
    const Track* track = clip != nullptr ? set.findTrack(clip->trackId) : nullptr;
    if (track == nullptr || param == Param::Pitch || moves.empty())
        return false;
    const ClipTiming timing(*clip, *track);
    auto sorted = moves;
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.seconds < b.seconds; });
    const double from = std::max(sorted.front().seconds, timing.startSeconds());
    const double to = std::min(endSeconds, timing.endSeconds());
    if (!(to >= from))
        return false;

    // The pass within the clip, holding the last value to the end.
    std::vector<RecordedMove> pass{{from, sorted.front().value}};
    for (const auto& move : sorted)
    {
        if (!std::isfinite(move.value) || move.seconds > to)
            continue;
        if (move.seconds <= from)
            pass.front().value = move.value;
        else
            pass.push_back(move);
    }
    if (pass.back().seconds < to)
        pass.push_back({to, pass.back().value});
    for (auto& move : pass)
        move.value = clampValue(param, move.value);
    const auto range = paramRange(param);
    pass = thinMoves(pass, kRecordingTolerance * (range.max - range.min));

    const Clip old = *clip;
    auto frameAt = [&](double seconds) { return static_cast<int64_t>(std::llround(timing.sourceFrameAt(seconds))); };
    std::optional<AutomationPoint> back;
    if (to + kRecordingReturnSeconds < timing.endSeconds())
    {
        const int64_t frame = frameAt(to + kRecordingReturnSeconds);
        back = AutomationPoint{frame, valueAt(old, param, static_cast<double>(frame))};
    }
    const int64_t first = frameAt(from), last = back ? back->sourceFrame : frameAt(to);
    auto& points = clip->points(param);
    std::erase_if(points, [&](const auto& p) { return p.sourceFrame >= first && p.sourceFrame <= last; });
    for (size_t i = 0; i < pass.size(); ++i)
    {
        if (i == 0 && pass[i].seconds - timing.startSeconds() <= kKeypointSnapSeconds)
            mixerValue(clip->mixer, param) = pass[i].value;
        else
            points.push_back({frameAt(pass[i].seconds), pass[i].value});
    }
    if (back)
        points.push_back(*back);
    sortPoints(points);
    return true;
}

double amountAt(const std::vector<RecordedMove>& moves, double seconds)
{
    if (moves.empty())
        return 0.0;
    auto after = std::upper_bound(moves.begin(), moves.end(), seconds,
                                  [](double s, const RecordedMove& m) { return s < m.seconds; });
    if (after == moves.begin())
        return moves.front().value;
    if (after == moves.end())
        return moves.back().value;
    const auto before = std::prev(after);
    const double span = after->seconds - before->seconds;
    const double t = span > 0.0 ? (seconds - before->seconds) / span : 1.0;
    return before->value + t * (after->value - before->value);
}

bool writeTrimmedPass(Set& set, Id clipId, Param param, const std::vector<RecordedMove>& amounts, double endSeconds,
                      const TrimLaw& law)
{
    const Clip* clip = findClip(set, clipId);
    const Track* track = clip != nullptr ? set.findTrack(clip->trackId) : nullptr;
    if (track == nullptr || param == Param::Pitch || amounts.empty() || !law)
        return false;
    const ClipTiming timing(*clip, *track);
    const double from = std::max(amounts.front().seconds, timing.startSeconds());
    const double to = std::min(endSeconds, timing.endSeconds());
    if (!(to >= from))
        return false;

    // The adjusted curve, sampled at the pass's moves, either side of the old keypoints (so jumps
    // stay sharp), and every few milliseconds between (the law bends straight lines).
    constexpr double kStep = 0.02, kEither = 0.001;
    std::vector<double> times{from, to};
    for (const auto& move : amounts)
        times.push_back(move.seconds);
    for (const auto& point : clip->points(param))
    {
        const double t = timing.timelineSecondsAt(static_cast<double>(point.sourceFrame));
        times.insert(times.end(), {t - kEither, t + kEither});
    }
    for (double t = from; t < to; t += kStep)
        times.push_back(t);
    std::erase_if(times, [&](double t) { return t < from || t > to; });
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());

    std::vector<RecordedMove> adjusted;
    adjusted.reserve(times.size());
    for (double t : times)
        adjusted.push_back({t, law(param, valueAt(*clip, param, timing.sourceFrameAt(t)), amountAt(amounts, t))});
    return writeRecordedPass(set, clipId, param, adjusted, to);
}

bool trimAutomation(Set& set, Id clipId, Param param, double amount, const TrimLaw& law)
{
    Clip* clip = findClip(set, clipId);
    if (clip == nullptr || param == Param::Pitch || !law || !std::isfinite(amount))
        return false;
    auto& start = mixerValue(clip->mixer, param);
    start = clampValue(param, law(param, start, amount));
    for (auto& point : clip->points(param))
        point.value = clampValue(param, law(param, point.value, amount));
    return true;
}

bool setClipMixer(Set& set, Id clipId, const ChannelSettings& settings)
{
    Clip* clip = findClip(set, clipId);
    if (clip == nullptr)
        return false;
    clip->mixer = {clamp01(settings.fader), clamp01(settings.high), clamp01(settings.mid), clamp01(settings.low),
                   clamp01(settings.trim), clamp01(settings.color)};
    return true;
}

bool setClipTrim(Set& set, Id clipId, double trim)
{
    Clip* clip = findClip(set, clipId);
    if (clip == nullptr)
        return false;
    clip->mixer.trim = clamp01(trim);
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
    // Keep the stored offset small, moving it by whole 16-bar phrases so bar and phrase lines
    // stay where they were.
    if (grid)
    {
        const double phrase = kBeatsPerPhrase * grid->periodSeconds();
        grid->firstBeatSeconds -= std::floor(grid->firstBeatSeconds / phrase) * phrase;
    }
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

namespace
{
// Shifts the clip's pitch (starting value and every keypoint) by whole steps, within range.
void shiftPitch(Clip& clip, int steps)
{
    const auto shifted = [steps](double value) {
        return std::clamp(value + steps, double(-kMaxPitchSteps), double(kMaxPitchSteps));
    };
    clip.pitchSteps = shifted(clip.pitchSteps);
    for (auto& point : clip.points(Param::Pitch))
        point.value = shifted(point.value);
}
} // namespace

bool withFollowers(Set& set, Id clipId, const std::function<bool(Set&)>& change)
{
    const Set before = set;
    if (!change(set))
        return false;

    struct Old
    {
        const Clip* clip;
        ClipTiming timing;
    };
    std::vector<Old> old; // the set before the change, in start order
    for (const auto& clip : before.clips)
        if (const Track* track = before.findTrack(clip.trackId))
            if (auto timing = timingFor(clip, *track))
                old.push_back({&clip, *timing});
    std::stable_sort(old.begin(), old.end(),
                     [](const Old& a, const Old& b) { return a.timing.startSeconds() < b.timing.startSeconds(); });

    auto edited = std::find_if(old.begin(), old.end(), [clipId](const Old& o) { return o.clip->id == clipId; });
    if (edited == old.end())
        return true;
    std::vector<Id> moved{clipId}; // the edited clip and its followers so far
    auto isMoved = [&](Id id) { return std::find(moved.begin(), moved.end(), id) != moved.end(); };
    auto newTiming = [&](Id id) -> std::optional<ClipTiming> {
        const Clip* clip = findClip(set, id);
        const Track* track = clip != nullptr ? set.findTrack(clip->trackId) : nullptr;
        return track != nullptr ? timingFor(*clip, *track) : std::nullopt;
    };

    for (auto x = std::next(edited); x != old.end(); ++x)
    {
        const double start = x->timing.startSeconds();
        if (start <= edited->timing.startSeconds())
            continue;

        // What it mixes into: the most recent clip on another deck still playing when it starts.
        const Old* parent = nullptr;
        for (auto c = old.begin(); c != x; ++c)
            if (c->clip->deck != x->clip->deck && c->timing.startSeconds() < start && start < c->timing.endSeconds())
                parent = &*c;

        Clip* clip = findClip(set, x->clip->id);
        if (clip == nullptr)
            continue;

        if (parent != nullptr)
        {
            const auto parentNow = newTiming(parent->clip->id);
            if (!isMoved(parent->clip->id) || !parentNow)
                continue;
            // Keep the two records' music together in the middle of the overlap.
            const double anchor = 0.5 * (start + std::min(parent->timing.endSeconds(), x->timing.endSeconds()));
            const double parentFrame = parent->timing.sourceFrameAt(anchor);
            const double ownFrame = x->timing.sourceFrameAt(anchor);
            const double anchorNow = parentNow->timelineSecondsAt(parentFrame);

            const double ratio = parentNow->speedAt(anchorNow) / parent->timing.speedAt(anchor);
            const double ownSteps = pitchStepsFromSpeed(x->timing.speedAt(anchor));
            const auto steps = static_cast<int>(std::lround(pitchStepsFromSpeed(x->timing.speedAt(anchor) * ratio) - ownSteps));
            shiftPitch(*clip, steps);

            const auto ownNow = newTiming(clip->id);
            if (!ownNow)
                continue;
            clip->startSeconds += anchorNow - ownNow->timelineSecondsAt(ownFrame);
        }
        else
        {
            // After a gap: keep the gap to whatever ended last before it.
            const Old* previous = nullptr;
            for (auto c = old.begin(); c != x; ++c)
                if (c->timing.endSeconds() <= start &&
                    (previous == nullptr || c->timing.endSeconds() > previous->timing.endSeconds()))
                    previous = &*c;
            if (previous == nullptr || !isMoved(previous->clip->id))
                continue;
            const auto previousNow = newTiming(previous->clip->id);
            if (!previousNow)
                continue;
            clip->startSeconds += previousNow->endSeconds() - previous->timing.endSeconds();
        }
        moved.push_back(clip->id);
    }
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

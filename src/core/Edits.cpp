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

// Deck actions (nudges, touches) correct a record against another at the tempo and place they were
// made at: changing its pitch, or moving it, leaves them meaning nothing, so they go.
void dropActions(Clip& clip, Param changed = Param::Pitch)
{
    if (changed == Param::Pitch)
        clip.actions.clear();
}

void insertAction(Clip& clip, DeckAction action)
{
    const auto at = std::upper_bound(clip.actions.begin(), clip.actions.end(), action.sourceFrame,
                                     [](int64_t f, const DeckAction& a) { return f < a.sourceFrame; });
    clip.actions.insert(at, std::move(action));
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
    // A record following beat links is placed by them (solveLinks): only its deck changes, and
    // never to a deck it's linked with.
    for (const auto& link : set.links)
    {
        if (link.follower == clipId)
            startSeconds = clip->startSeconds;
        const Id other = link.follower == clipId ? link.leader : link.leader == clipId ? link.follower : 0;
        if (const Clip* o = other != 0 ? set.findClip(other) : nullptr; o != nullptr && o->deck == deck)
            return false;
    }
    if (startSeconds != clip->startSeconds)
        dropActions(*clip);
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
            clip->pitchSteps = pitchStepsFromSpeed(timing->pitchSpeedAt(newStart));
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
    dropActions(*clip);
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
    dropActions(*clip, param);
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
    dropActions(*clip, param);
    return true;
}

bool deleteAutomationPoint(Set& set, Id clipId, Param param, size_t index)
{
    Clip* clip = findClip(set, clipId);
    if (clip == nullptr || index >= clip->points(param).size())
        return false;
    auto& points = clip->points(param);
    points.erase(points.begin() + static_cast<long>(index));
    dropActions(*clip, param);
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
    dropActions(*clip, param);
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
    dropActions(*clip, param);
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
    dropActions(*clip, param);
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

bool addNudge(Set& set, Id clipId, double atSeconds, double seconds)
{
    Clip* clip = findClip(set, clipId);
    const Track* track = clip != nullptr ? set.findTrack(clip->trackId) : nullptr;
    if (track == nullptr || !std::isfinite(atSeconds) || !std::isfinite(seconds) || seconds == 0.0)
        return false;
    const auto timing = timingFor(*clip, *track);
    if (!timing || atSeconds < timing->startSeconds() || atSeconds >= timing->endSeconds())
        return false;

    // Pressed again while the last nudge is still playing out: it carries on further, as a hand
    // already on the platter would.
    auto& actions = clip->actions;
    for (auto it = actions.begin(); it != actions.end(); ++it)
    {
        if (it->sourceFrame < clip->inFrame || it->kind != DeckAction::Kind::Nudge)
            continue;
        const double from = timing->timelineSecondsAt(static_cast<double>(it->sourceFrame));
        if (atSeconds < from || atSeconds > from + actionLengthSeconds(*it))
            continue;
        it->seconds = std::clamp(it->seconds + seconds, -kMaxNudgeSeconds, kMaxNudgeSeconds);
        if (std::abs(it->seconds) < 1e-6)
            actions.erase(it);
        return true;
    }
    DeckAction nudge;
    nudge.sourceFrame = std::clamp<int64_t>(std::llround(timing->sourceFrameAt(atSeconds)), clip->inFrame, clip->outFrame - 1);
    nudge.seconds = std::clamp(seconds, -kMaxNudgeSeconds, kMaxNudgeSeconds);
    insertAction(*clip, std::move(nudge));
    return true;
}

std::optional<double> beatOffset(const Set& set, Id clipId, const BeatGrid& grid, Id referenceId,
                                 const BeatGrid& referenceGrid, double atSeconds)
{
    const Clip* clip = set.findClip(clipId);
    const Clip* reference = set.findClip(referenceId);
    const Track* track = clip != nullptr ? set.findTrack(clip->trackId) : nullptr;
    const Track* referenceTrack = reference != nullptr ? set.findTrack(reference->trackId) : nullptr;
    if (track == nullptr || referenceTrack == nullptr || clipId == referenceId)
        return std::nullopt;
    const auto timing = timingFor(*clip, *track);
    const auto referenceTiming = timingFor(*reference, *referenceTrack);
    const auto plays = [](const ClipTiming& t, double s) { return s >= t.startSeconds() && s < t.endSeconds(); };
    if (!timing || !referenceTiming || !plays(*timing, atSeconds) || !plays(*referenceTiming, atSeconds))
        return std::nullopt;

    // Our next beat (not the nearest: that could be one a nudge before `atSeconds` is still moving),
    // and the reference's beat nearest to it.
    const auto beatTime = [](const ClipTiming& t, const Track& tr, const BeatGrid& g, double s, bool next) {
        const double at = g.beatAt(t.sourceFrameAt(s) / tr.sampleRate);
        const double beat = next ? std::ceil(at) : std::round(at);
        return t.timelineSecondsAt(g.secondsOfBeat(beat) * tr.sampleRate);
    };
    const double ours = beatTime(*timing, *track, grid, atSeconds, true);
    if (!plays(*referenceTiming, ours))
        return std::nullopt;
    return beatTime(*referenceTiming, *referenceTrack, referenceGrid, ours, false) - ours;
}

std::vector<SpeedPoint> cleanTouch(std::vector<SpeedPoint> points)
{
    std::vector<SpeedPoint> touch{{0.0, 1.0}};
    for (auto point : points)
    {
        if (!std::isfinite(point.seconds) || !std::isfinite(point.factor) || point.seconds <= touch.back().seconds)
            continue;
        if (point.seconds > kMaxTouchSeconds)
            break;
        point.factor = std::clamp(point.factor, 0.0, kMaxTouchFactor);
        touch.push_back(point);
    }
    // Let go: back up to speed.
    if (touch.back().factor != 1.0)
        touch.push_back({touch.back().seconds + kTouchReleaseSeconds * std::abs(1.0 - touch.back().factor), 1.0});
    return touch;
}

bool addTouch(Set& set, Id clipId, double atSeconds, std::vector<SpeedPoint> speeds)
{
    Clip* clip = findClip(set, clipId);
    const Track* track = clip != nullptr ? set.findTrack(clip->trackId) : nullptr;
    if (track == nullptr || !std::isfinite(atSeconds))
        return false;
    const auto timing = timingFor(*clip, *track);
    if (!timing || atSeconds < timing->startSeconds() || atSeconds >= timing->endSeconds())
        return false;
    DeckAction touch;
    touch.kind = DeckAction::Kind::Touch;
    touch.sourceFrame = std::clamp<int64_t>(std::llround(timing->sourceFrameAt(atSeconds)), clip->inFrame, clip->outFrame - 1);
    touch.touch = cleanTouch(std::move(speeds));
    if (touch.touch.size() < 2)
        return false;
    insertAction(*clip, std::move(touch));
    return true;
}

namespace
{
// A touch that holds the record at `factor` of its speed (0: still; above 1: pushed on) long enough
// to move the music after it by `seconds` (positive later, negative earlier), easing in and out.
std::vector<SpeedPoint> steadyTouch(double seconds, double factor)
{
    constexpr double kEaseIn = 0.02;
    const double easeOut = kTouchReleaseSeconds * std::abs(1.0 - factor); // added by cleanTouch
    const double length = std::max(0.0, seconds / (1.0 - factor) - kEaseIn / 2.0 - easeOut / 2.0);
    return cleanTouch({{kEaseIn, factor}, {kEaseIn + length, factor}});
}
} // namespace

bool addHold(Set& set, Id clipId, double atSeconds, double seconds)
{
    return seconds > 0.0 && addTouch(set, clipId, atSeconds, steadyTouch(seconds, 0.0));
}

std::optional<double> tidyActions(Set& set, Id clipId, double fromSeconds, double toSeconds)
{
    Clip* clip = findClip(set, clipId);
    const Track* track = clip != nullptr ? set.findTrack(clip->trackId) : nullptr;
    if (track == nullptr || !(toSeconds > fromSeconds))
        return std::nullopt;
    const auto timing = timingFor(*clip, *track);
    if (!timing)
        return std::nullopt;
    fromSeconds = std::max(fromSeconds, timing->startSeconds());

    // The actions starting in the stretch, and how far they move the music between them.
    double net = 0.0;
    bool any = false;
    std::vector<DeckAction> kept;
    for (const auto& action : clip->actions)
    {
        const double at = timing->timelineSecondsAt(static_cast<double>(action.sourceFrame));
        if (action.sourceFrame >= clip->inFrame && at >= fromSeconds && at <= toSeconds)
        {
            net += actionOffsetSeconds(action);
            any = true;
        }
        else
            kept.push_back(action);
    }
    if (!any || fromSeconds >= timing->endSeconds())
        return std::nullopt;
    clip->actions = std::move(kept);

    // All of it at the start of the stretch, as one move; nothing if they cancelled out. Removing
    // only actions after fromSeconds leaves the music there where it was.
    if (std::abs(net) >= 1e-4)
    {
        DeckAction move;
        move.sourceFrame = std::clamp<int64_t>(std::llround(timing->sourceFrameAt(fromSeconds)), clip->inFrame, clip->outFrame - 1);
        if (std::abs(net) <= kMaxNudgeSeconds)
            move.seconds = net;
        else
        {
            // Too far for a nudge: held still, or pushed on at half again its speed.
            move.kind = DeckAction::Kind::Touch;
            move.touch = steadyTouch(net, net > 0.0 ? 0.0 : 1.5);
        }
        insertAction(*clip, std::move(move));
    }
    return net;
}

bool setActionSeconds(Set& set, Id clipId, size_t index, double seconds)
{
    Clip* clip = findClip(set, clipId);
    if (clip == nullptr || index >= clip->actions.size() || !std::isfinite(seconds))
        return false;
    if (seconds == 0.0)
        clip->actions.erase(clip->actions.begin() + static_cast<std::ptrdiff_t>(index));
    else if (clip->actions[index].kind == DeckAction::Kind::Nudge)
        clip->actions[index].seconds = std::clamp(seconds, -kMaxNudgeSeconds, kMaxNudgeSeconds);
    else
        return false;
    return true;
}

bool setActionOffset(Set& set, Id clipId, size_t index, double seconds)
{
    Clip* clip = findClip(set, clipId);
    if (clip == nullptr || index >= clip->actions.size() || !std::isfinite(seconds))
        return false;
    auto& action = clip->actions[index];
    if (action.kind == DeckAction::Kind::Nudge)
    {
        action.seconds = std::clamp(seconds, -kMaxNudgeSeconds, kMaxNudgeSeconds);
        return true;
    }
    // A touch keeps its shape, stretched in time; it can't be taken through zero or the other way.
    const double current = actionOffsetSeconds(action);
    if (std::abs(current) < 1e-9 || seconds / current < 0.05)
        return false;
    const double stretch = std::min(seconds / current, kMaxTouchSeconds / actionLengthSeconds(action));
    for (auto& point : action.touch)
        point.seconds *= stretch;
    return true;
}

bool setNudgeSpeed(Set& set, Id clipId, size_t index, double speed)
{
    Clip* clip = findClip(set, clipId);
    if (clip == nullptr || index >= clip->actions.size() || clip->actions[index].kind != DeckAction::Kind::Nudge ||
        !std::isfinite(speed))
        return false;
    clip->actions[index].speed = std::clamp(speed, kMinNudgeSpeed, kMaxNudgeSpeed);
    return true;
}

bool moveAction(Set& set, Id clipId, size_t index, int64_t sourceFrame)
{
    Clip* clip = findClip(set, clipId);
    if (clip == nullptr || index >= clip->actions.size() || clip->outFrame <= clip->inFrame)
        return false;
    // Stays between its neighbours so the order (and the caller's index) holds.
    const auto& actions = clip->actions;
    const int64_t low = std::max(clip->inFrame, index > 0 ? actions[index - 1].sourceFrame : clip->inFrame);
    const int64_t high = std::min(clip->outFrame - 1, index + 1 < actions.size() ? actions[index + 1].sourceFrame : clip->outFrame - 1);
    clip->actions[index].sourceFrame = std::clamp(sourceFrame, low, std::max(low, high));
    return true;
}

bool deleteAction(Set& set, Id clipId, size_t index)
{
    return setActionSeconds(set, clipId, index, 0.0);
}

bool clearActions(Set& set, Id clipId)
{
    Clip* clip = findClip(set, clipId);
    if (clip == nullptr || clip->actions.empty())
        return false;
    clip->actions.clear();
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
    if (grid && (!grid->valid() || grid->bpm < 20.0 || grid->bpm > 400.0))
        return false;
    // Keep the stored offset small: beat 0 on the first phrase start at or after the start of the
    // file, renumbering by whole 16-bar phrases so bar and phrase lines stay where they were.
    if (grid)
    {
        const double phrases = std::ceil(grid->beatAt(0.0) / kBeatsPerPhrase);
        if (std::isfinite(phrases) && std::abs(phrases) < 1e9)
            grid = grid->renumbered(static_cast<int64_t>(phrases) * kBeatsPerPhrase);
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

        // After a gap it mixes into nothing, and stays put.
        if (parent == nullptr)
            continue;
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

namespace
{
std::optional<ClipTiming> timingOf(const Set& set, Id clipId)
{
    const Clip* clip = set.findClip(clipId);
    const Track* track = clip != nullptr ? set.findTrack(clip->trackId) : nullptr;
    return track != nullptr ? timingFor(*clip, *track) : std::nullopt;
}

bool inMusic(const Clip& clip, int64_t frame)
{
    return frame >= clip.inFrame && frame < clip.outFrame;
}

// A follower's links it can keep, in the order of its music: each heard after the one before.
struct Target
{
    int64_t frame;  // in the follower
    double seconds; // when the leader plays its end
};
std::vector<Target> targetsFor(const Set& set, Id follower)
{
    const Clip* clip = set.findClip(follower);
    std::vector<Target> targets;
    if (clip == nullptr)
        return targets;
    for (const auto& link : set.links)
    {
        const Clip* leader = set.findClip(link.leader);
        if (link.follower != follower || leader == nullptr || !inMusic(*clip, link.followerFrame) ||
            !inMusic(*leader, link.leaderFrame))
            continue;
        if (const auto timing = timingOf(set, link.leader))
            targets.push_back({link.followerFrame, timing->timelineSecondsAt(static_cast<double>(link.leaderFrame))});
    }
    std::sort(targets.begin(), targets.end(), [](const Target& a, const Target& b) { return a.frame < b.frame; });
    std::vector<Target> kept;
    for (const auto& t : targets)
        if (kept.empty() || (t.frame > kept.back().frame && t.seconds > kept.back().seconds))
            kept.push_back(t);
    return kept;
}

bool crosses(const Set& set, Id follower)
{
    size_t count = 0;
    for (const auto& link : set.links)
        count += link.follower == follower ? 1 : 0;
    return targetsFor(set, follower).size() != count;
}

bool solveFollower(Set& set, Id followerId)
{
    constexpr double kGap = 0.02;          // between a link and the nudge before or after it
    constexpr double kCloseEnough = 0.00005;
    Clip* clip = findClip(set, followerId);
    const Track* track = clip != nullptr ? set.findTrack(clip->trackId) : nullptr;
    if (track == nullptr)
        return false;
    const Clip before = *clip;
    std::erase_if(clip->actions, [](const DeckAction& a) { return a.linked; });
    const auto targets = targetsFor(set, followerId);
    if (targets.empty())
        return !(*clip == before);

    // The pitch fader, with two or more links: one setting for as many links in a row as it keeps
    // within a nudge of each other, on the nearest step; where the music changes tempo, the fader
    // moves to the next setting just after the link.
    if (targets.size() >= 2)
    {
        constexpr double kNudgeable = 0.05;  // drift between two links that a nudge makes up
        constexpr double kRideSeconds = 0.25; // how long the fader takes to move, in the music
        const auto stepsFor = [&](size_t a, size_t b) {
            const double music = static_cast<double>(targets[b].frame - targets[a].frame) / track->sampleRate;
            const double speed = music / (targets[b].seconds - targets[a].seconds);
            return std::clamp(std::round(pitchStepsFromSpeed(speed)), double(-kMaxPitchSteps), double(kMaxPitchSteps));
        };
        const auto holds = [&](size_t a, size_t b, double steps) {
            for (size_t i = a; i < b; ++i)
            {
                const double music = static_cast<double>(targets[i + 1].frame - targets[i].frame) / track->sampleRate;
                if (std::abs(music / speedFromPitchSteps(steps) - (targets[i + 1].seconds - targets[i].seconds)) > kNudgeable)
                    return false;
            }
            return true;
        };
        auto& points = clip->points(Param::Pitch);
        points.clear();
        size_t a = 0;
        double steps = 0.0;
        while (a + 1 < targets.size())
        {
            size_t b = a + 1;
            while (b + 1 < targets.size() && holds(a, b + 1, stepsFor(a, b + 1)))
                ++b;
            const double previous = steps;
            steps = stepsFor(a, b);
            if (a == 0)
                clip->pitchSteps = steps;
            else if (steps != previous)
            {
                const int64_t from = targets[a].frame;
                const auto ride = std::min(static_cast<int64_t>(kRideSeconds * track->sampleRate),
                                           (targets[a + 1].frame - from) / 2);
                points.push_back({from, previous});
                points.push_back({from + ride, steps});
            }
            a = b;
        }
    }
    // The first link heard together: where the needle drops.
    if (const auto timing = timingFor(*clip, *track))
    {
        const double shift = targets.front().seconds - timing->timelineSecondsAt(static_cast<double>(targets.front().frame));
        if (std::abs(shift) > 1e-7)
            clip->startSeconds += shift;
    }
    // Each further link: a nudge just before it makes up the difference.
    for (size_t i = 1; i < targets.size(); ++i)
    {
        const auto timing = timingFor(*clip, *track);
        if (!timing)
            break;
        const double heard = timing->timelineSecondsAt(static_cast<double>(targets[i].frame));
        const double seconds = std::clamp(targets[i].seconds - heard, -kMaxNudgeSeconds, kMaxNudgeSeconds);
        if (std::abs(seconds) < kCloseEnough)
            continue;
        const double from = targets[i - 1].seconds + kGap;
        const double room = heard - kGap - from;
        double speed = kNudgeSpeed;
        if (room > 0.0 && nudgeLengthSeconds(seconds, speed) > room)
            speed = std::min(kMaxNudgeSpeed, speed * nudgeLengthSeconds(seconds, speed) / room);
        DeckAction nudge;
        nudge.seconds = seconds;
        nudge.speed = speed;
        nudge.linked = true;
        const double at = std::max(from, heard - kGap - nudgeLengthSeconds(seconds, speed));
        nudge.sourceFrame = std::clamp<int64_t>(std::llround(timing->sourceFrameAt(at)), clip->inFrame, clip->outFrame - 1);
        insertAction(*clip, std::move(nudge));
    }
    return !(*clip == before);
}
} // namespace

std::optional<Id> lineUpBeats(Set& set, Id clipId, const BeatGrid& grid, Id referenceId, const BeatGrid& referenceGrid,
                              double atSeconds)
{
    const Clip* clip = set.findClip(clipId);
    const Clip* reference = set.findClip(referenceId);
    const Track* track = clip != nullptr ? set.findTrack(clip->trackId) : nullptr;
    const Track* referenceTrack = reference != nullptr ? set.findTrack(reference->trackId) : nullptr;
    const auto timing = timingOf(set, clipId);
    const auto referenceTiming = timingOf(set, referenceId);
    if (track == nullptr || referenceTrack == nullptr || !timing || !referenceTiming || atSeconds < timing->startSeconds())
        return std::nullopt;
    const double beat = std::ceil(grid.beatAt(timing->sourceFrameAt(atSeconds) / track->sampleRate));
    const double frame = grid.secondsOfBeat(beat) * track->sampleRate;
    const double heard = timing->timelineSecondsAt(frame);
    if (heard < referenceTiming->startSeconds() || heard >= referenceTiming->endSeconds())
        return std::nullopt;
    const double referenceBeat =
        std::round(referenceGrid.beatAt(referenceTiming->sourceFrameAt(heard) / referenceTrack->sampleRate));
    const double referenceFrame = referenceGrid.secondsOfBeat(referenceBeat) * referenceTrack->sampleRate;
    return addLink(set, clipId, std::llround(frame), referenceId, std::llround(referenceFrame));
}

std::optional<Id> addLink(Set& set, Id clipA, int64_t frameA, Id clipB, int64_t frameB)
{
    const Clip* a = set.findClip(clipA);
    const Clip* b = set.findClip(clipB);
    if (a == nullptr || b == nullptr || a->deck == b->deck || !inMusic(*a, frameA) || !inMusic(*b, frameB))
        return std::nullopt;
    BeatLink link;
    link.id = set.allocateId();
    const bool bFollows = b->startSeconds >= a->startSeconds;
    link.leader = bFollows ? clipA : clipB;
    link.leaderFrame = bFollows ? frameA : frameB;
    link.follower = bFollows ? clipB : clipA;
    link.followerFrame = bFollows ? frameB : frameA;

    Set trial = set;
    const Track* track = set.findTrack(set.findClip(link.follower)->trackId);
    const auto near = static_cast<int64_t>(0.05 * track->sampleRate);
    std::erase_if(trial.links, [&](const BeatLink& l) {
        return l.follower == link.follower && std::abs(l.followerFrame - link.followerFrame) < near;
    });
    trial.links.push_back(link);
    if (crosses(trial, link.follower))
        return std::nullopt;
    set = std::move(trial);
    return link.id;
}

bool moveLink(Set& set, Id linkId, Id clipId, int64_t frame)
{
    auto it = std::find_if(set.links.begin(), set.links.end(), [linkId](const BeatLink& l) { return l.id == linkId; });
    const Clip* clip = set.findClip(clipId);
    if (it == set.links.end() || clip == nullptr || !inMusic(*clip, frame) ||
        (it->leader != clipId && it->follower != clipId))
        return false;
    const BeatLink old = *it;
    (it->leader == clipId ? it->leaderFrame : it->followerFrame) = frame;
    if (crosses(set, it->follower))
    {
        *it = old;
        return false;
    }
    return !(*it == old);
}

bool deleteLink(Set& set, Id linkId)
{
    return std::erase_if(set.links, [linkId](const BeatLink& l) { return l.id == linkId; }) > 0;
}

bool deleteLinks(Set& set, Id clipId)
{
    return std::erase_if(set.links, [clipId](const BeatLink& l) { return l.leader == clipId || l.follower == clipId; }) > 0;
}

void solveLinks(Set& set)
{
    std::erase_if(set.links, [&](const BeatLink& l) {
        return set.findClip(l.leader) == nullptr || set.findClip(l.follower) == nullptr;
    });
    // Followers, and clips still carrying nudges from links since deleted.
    std::vector<Id> followers;
    for (const auto& link : set.links)
        followers.push_back(link.follower);
    for (const auto& clip : set.clips)
        if (std::any_of(clip.actions.begin(), clip.actions.end(), [](const DeckAction& a) { return a.linked; }))
            followers.push_back(clip.id);
    std::sort(followers.begin(), followers.end());
    followers.erase(std::unique(followers.begin(), followers.end()), followers.end());
    const auto startOf = [&](Id id) { return set.findClip(id)->startSeconds; };
    std::stable_sort(followers.begin(), followers.end(), [&](Id x, Id y) { return startOf(x) < startOf(y); });
    for (const Id id : followers)
        withFollowers(set, id, [id](Set& s) { return solveFollower(s, id); });
}

std::optional<double> linkError(const Set& set, const BeatLink& link)
{
    const Clip* leader = set.findClip(link.leader);
    const Clip* follower = set.findClip(link.follower);
    if (leader == nullptr || follower == nullptr || !inMusic(*leader, link.leaderFrame) ||
        !inMusic(*follower, link.followerFrame))
        return std::nullopt;
    const auto a = timingOf(set, link.leader);
    const auto b = timingOf(set, link.follower);
    if (!a || !b)
        return std::nullopt;
    return b->timelineSecondsAt(static_cast<double>(link.followerFrame)) -
           a->timelineSecondsAt(static_cast<double>(link.leaderFrame));
}

} // namespace vmp::edit


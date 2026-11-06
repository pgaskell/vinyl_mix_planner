#include "core/Set.h"

#include "core/Pitch.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace vmp
{

const Track* Set::findTrack(Id id) const
{
    auto it = std::find_if(tracks.begin(), tracks.end(), [id](const Track& t) { return t.id == id; });
    return it == tracks.end() ? nullptr : &*it;
}

const Clip* Set::findClip(Id id) const
{
    auto it = std::find_if(clips.begin(), clips.end(), [id](const Clip& c) { return c.id == id; });
    return it == clips.end() ? nullptr : &*it;
}

// --- Automation -------------------------------------------------------------

ParamRange paramRange(Param param)
{
    if (param == Param::Pitch)
        return {-kMaxPitchSteps, kMaxPitchSteps};
    return {0.0, 1.0};
}

const char* paramName(Param param)
{
    switch (param)
    {
    case Param::Pitch:
        return "Pitch";
    case Param::Volume:
        return "Volume";
    case Param::High:
        return "Hi";
    case Param::Mid:
        return "Mid";
    case Param::Low:
        return "Low";
    case Param::Color:
        return "Color";
    }
    return "?";
}

double baseValue(const Clip& clip, Param param)
{
    switch (param)
    {
    case Param::Pitch:
        return clip.pitchSteps;
    case Param::Volume:
        return clip.mixer.fader;
    case Param::High:
        return clip.mixer.high;
    case Param::Mid:
        return clip.mixer.mid;
    case Param::Low:
        return clip.mixer.low;
    case Param::Color:
        return clip.mixer.color;
    }
    return 0.0;
}

double valueAt(const Clip& clip, Param param, double sourceFrame)
{
    // The starting value is a fixed point at the needle drop; keypoints from there on follow.
    // Keypoints before the needle drop (trimmed away) are ignored.
    const auto inFrame = static_cast<double>(clip.inFrame);
    double fromFrame = inFrame, fromValue = baseValue(clip, param);
    if (sourceFrame < inFrame)
        return fromValue;
    for (const auto& point : clip.points(param))
    {
        const auto frame = static_cast<double>(point.sourceFrame);
        if (frame < inFrame)
            continue;
        if (sourceFrame < frame)
            return fromValue + (point.value - fromValue) * (sourceFrame - fromFrame) / (frame - fromFrame);
        fromFrame = frame;
        fromValue = point.value;
    }
    return fromValue;
}

ChannelSettings mixerAt(const Clip& clip, double sourceFrame)
{
    return {valueAt(clip, Param::Volume, sourceFrame), valueAt(clip, Param::High, sourceFrame),
            valueAt(clip, Param::Mid, sourceFrame), valueAt(clip, Param::Low, sourceFrame), clip.mixer.trim,
            valueAt(clip, Param::Color, sourceFrame)};
}

// --- Deck actions -----------------------------------------------------------

double nudgeLengthSeconds(double seconds, double speed)
{
    // The shape's area: the attack and the ease-off count half.
    const double fill = (kNudgeEaseFrom - kNudgeAttack) + kNudgeAttack / 2.0 + (1.0 - kNudgeEaseFrom) / 2.0;
    return std::abs(seconds) / (fill * std::clamp(speed, kMinNudgeSpeed, kMaxNudgeSpeed));
}

std::vector<SpeedPoint> speedShape(const DeckAction& action)
{
    if (action.kind == DeckAction::Kind::Touch)
        return action.touch.empty() ? std::vector<SpeedPoint>{{0.0, 1.0}} : action.touch;
    if (action.seconds == 0.0)
        return {{0.0, 1.0}};
    const double length = nudgeLengthSeconds(action.seconds, action.speed);
    const double peak = 1.0 + std::clamp(action.speed, kMinNudgeSpeed, kMaxNudgeSpeed) * (action.seconds < 0.0 ? 1.0 : -1.0);
    return {{0.0, 1.0}, {kNudgeAttack * length, peak}, {kNudgeEaseFrom * length, peak}, {length, 1.0}};
}

double actionLengthSeconds(const DeckAction& action)
{
    return speedShape(action).back().seconds;
}

double actionOffsetSeconds(const DeckAction& action)
{
    const auto shape = speedShape(action);
    double offset = 0.0;
    for (size_t i = 1; i < shape.size(); ++i)
        offset += (shape[i].seconds - shape[i - 1].seconds) * (1.0 - (shape[i].factor + shape[i - 1].factor) / 2.0);
    return offset;
}

// --- ClipTiming -------------------------------------------------------------

namespace
{
PitchCurve makeCurve(const Clip& clip, const Track& track)
{
    // Same shape as valueAt(): the starting pitch at the needle drop, then the keypoints after it.
    const double baseSpeed = speedFromPitchSteps(clip.pitchSteps);
    std::vector<PitchCurve::Point> points{{0.0, baseSpeed}};
    for (const auto& p : clip.points(Param::Pitch))
        if (p.sourceFrame >= clip.inFrame)
            points.push_back({static_cast<double>(p.sourceFrame - clip.inFrame) / track.sampleRate,
                              1.0 + p.value * kPitchStepPercent / 100.0});
    return PitchCurve(baseSpeed, std::move(points));
}
} // namespace

namespace
{
// What one action adds to timeline time, as a function of pitch time: points (pitch time, extra
// seconds), linear between them, from 0 at the first to the action's offset at the last. Points
// at one pitch time make a step (the record held still).
using Extra = std::vector<std::pair<double, double>>;

Extra extraFor(const DeckAction& action, double from)
{
    // Each stretch of the speed curve in short steps of its average speed: exact at the step ends,
    // and finer than the engine's blocks would hear.
    constexpr double kStepSeconds = 0.005;
    Extra extra{{from, 0.0}};
    double p = from, added = 0.0;
    const auto shape = speedShape(action);
    for (size_t i = 1; i < shape.size(); ++i)
    {
        const auto& a = shape[i - 1];
        const auto& b = shape[i];
        const double length = b.seconds - a.seconds;
        if (!(length > 0.0))
            continue;
        const int steps = std::max(1, static_cast<int>(std::ceil(length / kStepSeconds)));
        const double step = length / steps;
        for (int k = 0; k < steps; ++k)
        {
            const double factor = a.factor + (b.factor - a.factor) * (k + 0.5) / steps;
            p += factor * step;
            added += (1.0 - factor) * step;
            extra.push_back({p, added});
        }
    }
    return extra;
}

// The extra at pitch time p: approached from the right (after a step at p) or from the left.
double extraAt(const Extra& extra, double p, bool fromLeft)
{
    const auto it = fromLeft ? std::lower_bound(extra.begin(), extra.end(), p,
                                                [](const auto& point, double v) { return point.first < v; })
                             : std::upper_bound(extra.begin(), extra.end(), p,
                                                [](double v, const auto& point) { return v < point.first; });
    if (it == extra.begin())
        return 0.0;
    if (it == extra.end())
        return extra.back().second;
    const auto& [p0, e0] = *std::prev(it);
    const auto& [p1, e1] = *it;
    return p1 > p0 ? e0 + (p - p0) / (p1 - p0) * (e1 - e0) : e1;
}

// Linear interpolation in sorted breakpoints: the x's may repeat (a step), the y's don't.
double interpolate(const std::vector<double>& xs, const std::vector<double>& ys, double x)
{
    if (xs.empty() || x < xs.front())
        return x;
    const auto i = static_cast<size_t>(std::upper_bound(xs.begin(), xs.end(), x) - xs.begin());
    if (i == xs.size())
        return x + ys.back() - xs.back();
    const double fraction = (x - xs[i - 1]) / (xs[i] - xs[i - 1]);
    return ys[i - 1] + fraction * (ys[i] - ys[i - 1]);
}
} // namespace

ClipTiming::ClipTiming(const Clip& clip, const Track& track)
    : start_(clip.startSeconds), sampleRate_(track.sampleRate), inFrame_(static_cast<double>(clip.inFrame)),
      curve_(makeCurve(clip, track))
{
    // Each action starts where its music plays, in pitch time; overlapping ones add up.
    std::vector<Extra> extras;
    for (const auto& action : clip.actions)
        if (action.sourceFrame >= clip.inFrame)
            extras.push_back(extraFor(
                action, curve_.timeAtSourceSeconds((static_cast<double>(action.sourceFrame) - inFrame_) / sampleRate_)));
    std::sort(extras.begin(), extras.end(), [](const Extra& a, const Extra& b) { return a.front().first < b.front().first; });
    std::vector<double> breaks;
    for (const auto& extra : extras)
        for (const auto& point : extra)
            breaks.push_back(point.first);
    std::sort(breaks.begin(), breaks.end());
    breaks.erase(std::unique(breaks.begin(), breaks.end()), breaks.end());

    // Swept in order: at each breakpoint only the actions under way are looked into; those that
    // have finished just add their offset. (Looking into all of them at every breakpoint made this
    // quadratic, and it's built on every edit.)
    size_t next = 0;    // the first action not started yet
    double done = 0.0;  // the offsets of the finished ones
    std::vector<const Extra*> underWay;
    for (const double p : breaks)
    {
        while (next < extras.size() && extras[next].front().first <= p)
            underWay.push_back(&extras[next++]);
        double before = p + done, after = p + done;
        for (auto it = underWay.begin(); it != underWay.end();)
        {
            before += extraAt(**it, p, true);
            if ((*it)->back().first < p)
            {
                done += (*it)->back().second;
                after += (*it)->back().second;
                it = underWay.erase(it);
                continue;
            }
            after += extraAt(**it, p, false);
            ++it;
        }
        pitchTimes_.push_back(p);
        warpedTimes_.push_back(before);
        if (after > before)
        {
            pitchTimes_.push_back(p);
            warpedTimes_.push_back(after);
        }
    }
    end_ = timelineSecondsAt(static_cast<double>(clip.outFrame));
}

double ClipTiming::warp(double p) const
{
    return interpolate(pitchTimes_, warpedTimes_, p);
}

double ClipTiming::unwarp(double w) const
{
    return interpolate(warpedTimes_, pitchTimes_, w);
}

double ClipTiming::rateAt(double w) const
{
    if (warpedTimes_.empty() || w < warpedTimes_.front() || w >= warpedTimes_.back())
        return 1.0;
    const auto i = static_cast<size_t>(std::upper_bound(warpedTimes_.begin(), warpedTimes_.end(), w) - warpedTimes_.begin());
    return (pitchTimes_[i] - pitchTimes_[i - 1]) / (warpedTimes_[i] - warpedTimes_[i - 1]);
}

double ClipTiming::speedAt(double timelineSeconds) const
{
    const double w = timelineSeconds - start_;
    return curve_.speedAt(unwarp(w)) * rateAt(w);
}

double ClipTiming::pitchSpeedAt(double timelineSeconds) const
{
    return curve_.speedAt(unwarp(timelineSeconds - start_));
}

double ClipTiming::sourceFrameAt(double timelineSeconds) const
{
    return inFrame_ + sampleRate_ * curve_.sourceSecondsAt(unwarp(timelineSeconds - start_));
}

double ClipTiming::timelineSecondsAt(double sourceFrame) const
{
    return start_ + warp(curve_.timeAtSourceSeconds((sourceFrame - inFrame_) / sampleRate_));
}

// --- Validation -------------------------------------------------------------

namespace
{
std::string format(const char* fmt, double value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, fmt, value);
    return buffer;
}

std::string quoted(const Track& track)
{
    return "'" + (track.title.empty() ? track.path : track.title) + "'";
}

std::string deckName(int deck)
{
    return "Deck " + std::to_string(deck + 1);
}

bool pitchInRange(double steps)
{
    return std::abs(steps) <= kMaxPitchSteps;
}

struct Placed
{
    const Clip* clip;
    const Track* track;
    double start, end;
};
} // namespace

std::vector<Issue> validate(const Set& set)
{
    std::vector<Issue> issues;
    auto report = [&](Issue::Severity severity, std::string message, std::vector<Id> ids) {
        issues.push_back({severity, std::move(message), std::move(ids)});
    };
    constexpr auto kError = Issue::Severity::Error;
    constexpr auto kWarning = Issue::Severity::Warning;

    if (set.numDecks < 1 || set.numDecks > kMaxDecks)
        report(kError, "The set has " + std::to_string(set.numDecks) + " decks; 1 to 4 are supported.", {});

    std::vector<Placed> placed;
    for (const auto& clip : set.clips)
    {
        const Track* track = set.findTrack(clip.trackId);
        if (track == nullptr)
        {
            report(kError, "A clip refers to a track that is not in the set.", {clip.id});
            continue;
        }
        const auto name = quoted(*track);

        if (clip.deck < 0 || clip.deck >= set.numDecks)
        {
            report(kError, name + " is on deck " + std::to_string(clip.deck + 1) + ", but the set has " +
                               std::to_string(set.numDecks) + " decks.",
                   {clip.id});
            continue;
        }
        if (clip.inFrame < 0 || clip.outFrame > track->frames || clip.outFrame <= clip.inFrame)
        {
            report(kError, name + " has start/end points outside the track.", {clip.id});
            continue;
        }

        bool pitchOk = pitchInRange(clip.pitchSteps);
        for (const auto& point : clip.points(Param::Pitch))
            pitchOk = pitchOk && std::abs(point.value) <= kMaxPitchSteps;
        if (!pitchOk)
        {
            report(kError, name + " uses a pitch beyond the MK2's +/-8%.", {clip.id});
            continue;
        }

        const ClipTiming timing(clip, *track);
        placed.push_back({&clip, track, timing.startSeconds(), timing.endSeconds()});
    }

    // One record at a time per turntable, with time to change it.
    for (int deck = 0; deck < set.numDecks; ++deck)
    {
        std::vector<const Placed*> lane;
        for (const auto& p : placed)
            if (p.clip->deck == deck)
                lane.push_back(&p);
        std::sort(lane.begin(), lane.end(), [](const Placed* a, const Placed* b) { return a->start < b->start; });

        const Placed* latest = nullptr; // the clip that ends last so far
        for (const Placed* p : lane)
        {
            if (latest != nullptr)
            {
                const double gap = p->start - latest->end;
                if (gap < 0.0)
                    report(kError,
                           deckName(deck) + ": " + quoted(*latest->track) + " and " + quoted(*p->track) +
                               " overlap by " + format("%.1f", -gap) + " s.",
                           {latest->clip->id, p->clip->id});
                else if (gap < kMinChangeoverSeconds)
                    report(kWarning,
                           deckName(deck) + ": only " + format("%.1f", gap) + " s to change from " +
                               quoted(*latest->track) + " to " + quoted(*p->track) + ".",
                           {latest->clip->id, p->clip->id});
            }
            if (latest == nullptr || p->end > latest->end)
                latest = p;
        }
    }

    // The same record on two decks at once needs two copies.
    for (size_t i = 0; i < placed.size(); ++i)
        for (size_t j = i + 1; j < placed.size(); ++j)
        {
            const auto& a = placed[i];
            const auto& b = placed[j];
            if (a.track->id == b.track->id && a.clip->deck != b.clip->deck && a.start < b.end && b.start < a.end)
                report(kWarning,
                       quoted(*a.track) + " plays on " + deckName(a.clip->deck) + " and " + deckName(b.clip->deck) +
                           " at the same time; you need two copies.",
                       {a.clip->id, b.clip->id});
        }

    // Beat links edit::solveLinks couldn't keep.
    for (const auto& link : set.links)
    {
        const Clip* leader = set.findClip(link.leader);
        const Clip* follower = set.findClip(link.follower);
        const Track* leaderTrack = leader != nullptr ? set.findTrack(leader->trackId) : nullptr;
        const Track* followerTrack = follower != nullptr ? set.findTrack(follower->trackId) : nullptr;
        if (leaderTrack == nullptr || followerTrack == nullptr || leader->outFrame <= leader->inFrame ||
            follower->outFrame <= follower->inFrame)
            continue;
        const auto inside = [](const Clip& c, int64_t f) { return f >= c.inFrame && f < c.outFrame; };
        const std::string names = quoted(*followerTrack) + " and " + quoted(*leaderTrack);
        if (!inside(*leader, link.leaderFrame) || !inside(*follower, link.followerFrame))
        {
            report(kWarning, "A beat link between " + names + " is outside the part that plays.",
                   {follower->id, leader->id});
            continue;
        }
        const double apart =
            ClipTiming(*follower, *followerTrack).timelineSecondsAt(static_cast<double>(link.followerFrame)) -
            ClipTiming(*leader, *leaderTrack).timelineSecondsAt(static_cast<double>(link.leaderFrame));
        if (std::abs(apart) > 0.001)
        {
            char ms[32];
            std::snprintf(ms, sizeof ms, "%.1f ms", std::abs(apart) * 1000.0);
            report(kError,
                   "A beat link between " + names + " can't be kept (" + ms +
                       " apart): it crosses another, or needs more than the pitch fader and a nudge can do.",
                   {follower->id, leader->id});
        }
    }

    return issues;
}

} // namespace vmp

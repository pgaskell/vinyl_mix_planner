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
    }
    return 0.0;
}

double valueAt(const Clip& clip, Param param, double sourceFrame)
{
    const auto& points = clip.points(param);
    if (points.empty())
        return baseValue(clip, param);
    auto next = std::upper_bound(points.begin(), points.end(), sourceFrame,
                                 [](double frame, const AutomationPoint& p) { return frame < static_cast<double>(p.sourceFrame); });
    if (next == points.begin())
        return points.front().value;
    const auto& before = *std::prev(next);
    if (next == points.end())
        return before.value;
    const double span = static_cast<double>(next->sourceFrame - before.sourceFrame);
    const double fraction = (sourceFrame - static_cast<double>(before.sourceFrame)) / span;
    return before.value + (next->value - before.value) * fraction;
}

ChannelSettings mixerAt(const Clip& clip, double sourceFrame)
{
    return {valueAt(clip, Param::Volume, sourceFrame), valueAt(clip, Param::High, sourceFrame),
            valueAt(clip, Param::Mid, sourceFrame), valueAt(clip, Param::Low, sourceFrame)};
}

// --- ClipTiming -------------------------------------------------------------

namespace
{
PitchCurve makeCurve(const Clip& clip, const Track& track)
{
    std::vector<PitchCurve::Point> points;
    points.reserve(clip.points(Param::Pitch).size());
    for (const auto& p : clip.points(Param::Pitch))
        points.push_back({static_cast<double>(p.sourceFrame - clip.inFrame) / track.sampleRate,
                          1.0 + p.value * kPitchStepPercent / 100.0});
    return PitchCurve(speedFromPitchSteps(clip.pitchSteps), std::move(points));
}
} // namespace

ClipTiming::ClipTiming(const Clip& clip, const Track& track)
    : start_(clip.startSeconds), sampleRate_(track.sampleRate), inFrame_(static_cast<double>(clip.inFrame)),
      curve_(makeCurve(clip, track)), end_(timelineSecondsAt(static_cast<double>(clip.outFrame)))
{
}

double ClipTiming::speedAt(double timelineSeconds) const
{
    return curve_.speedAt(timelineSeconds - start_);
}

double ClipTiming::sourceFrameAt(double timelineSeconds) const
{
    return inFrame_ + sampleRate_ * curve_.sourceSecondsAt(timelineSeconds - start_);
}

double ClipTiming::timelineSecondsAt(double sourceFrame) const
{
    return start_ + curve_.timeAtSourceSeconds((sourceFrame - inFrame_) / sampleRate_);
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

bool pitchInRange(int steps)
{
    return steps >= -kMaxPitchSteps && steps <= kMaxPitchSteps;
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

    return issues;
}

} // namespace vmp

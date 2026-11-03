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

    return issues;
}

} // namespace vmp

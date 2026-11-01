#include "app/TimelineView.h"

#include "app/TimeFormat.h"
#include "core/Edits.h"
#include "core/Pitch.h"

#include <cmath>

namespace vmp
{

namespace
{
constexpr double kMinPixelsPerSecond = 0.02;  // ~1.5 hours across a laptop screen
constexpr double kMaxPixelsPerSecond = 48000; // one sample per pixel
constexpr double kMinTransitionSpan = 4.0;
constexpr double kFineDragFactor = 0.1;
constexpr double kMinBeatSpacingPixels = 8.0; // closer than this, beat lines are just noise
constexpr int kBeatsInBeatZoom = 8;
constexpr float kKeypointGrab = 6.0f;
constexpr int kTitleHeight = 18;
constexpr float kLineGrab = 5.0f;

juce::Colour paramColour(Param param)
{
    switch (param)
    {
    case Param::Pitch:
        return juce::Colour(0xffffe14d);
    case Param::Volume:
        return juce::Colour(0xff7ee08a);
    case Param::High:
        return juce::Colour(0xfff4f4f4);
    case Param::Mid:
        return juce::Colour(0xffffa31a);
    case Param::Low:
        return juce::Colour(0xff4f8cff);
    }
    return juce::Colours::white;
}

// CDJ-3000 style 3-band colours.
const juce::Colour kLowColour{0xff2a6cf0};
const juce::Colour kMidColour{0xffffa31a};
const juce::Colour kHighColour{0xfff4f4f4};

const juce::Colour kBackground{0xff1b1d21};
const juce::Colour kLaneBackground{0xff23262b};
const juce::Colour kRulerBackground{0xff2c3036};
const juce::Colour kDeckColours[] = {juce::Colour(0xff4fa3d9), juce::Colour(0xffd98f4f), juce::Colour(0xff69c27a),
                                     juce::Colour(0xffc06fd0)};

juce::Colour deckColour(int deck)
{
    return kDeckColours[static_cast<size_t>(std::clamp(deck, 0, kMaxDecks - 1))];
}
} // namespace

TimelineView::TimelineView(SetDocument& document, AudioPool& pool, Engine& engine, const DisplayOptions& display)
    : document_(document), pool_(pool), engine_(engine), display_(display)
{
    setWantsKeyboardFocus(true);
    static const char* const kShortNames[] = {"Pitch", "Vol", "Hi", "Mid", "Low"};
    for (int d = 0; d < kMaxDecks; ++d)
        for (int p = 0; p < kNumParams; ++p)
        {
            auto& toggle = toggles_[static_cast<size_t>(d)][static_cast<size_t>(p)];
            const auto param = static_cast<Param>(p);
            toggle.setButtonText(kShortNames[p]);
            toggle.setColour(juce::ToggleButton::tickColourId, paramColour(param));
            toggle.setColour(juce::ToggleButton::textColourId, paramColour(param));
            toggle.setWantsKeyboardFocus(false);
            toggle.setTooltip(juce::String("Show ") + paramName(param) + " automation on this deck");
            toggle.onClick = [this, d, param] {
                setShowsAutomation(d, param, toggles_[static_cast<size_t>(d)][static_cast<size_t>(param)].getToggleState());
            };
            addChildComponent(toggle);
        }
}

bool TimelineView::showsAutomation(int deck, Param param) const
{
    return shown_[static_cast<size_t>(deck)][static_cast<size_t>(param)];
}

bool TimelineView::anyAutomationShown(int deck) const
{
    const auto& row = shown_[static_cast<size_t>(deck)];
    return std::any_of(row.begin(), row.end(), [](bool b) { return b; });
}

void TimelineView::setShowsAutomation(int deck, Param param, bool show)
{
    shown_[static_cast<size_t>(deck)][static_cast<size_t>(param)] = show;
    toggles_[static_cast<size_t>(deck)][static_cast<size_t>(param)].setToggleState(show, juce::dontSendNotification);
    repaint();
}

void TimelineView::toggleAllAutomation(int deck)
{
    const bool show = !anyAutomationShown(deck);
    for (int p = 0; p < kNumParams; ++p)
        setShowsAutomation(deck, static_cast<Param>(p), show);
}

std::vector<std::pair<double, double>> TimelineView::mixes() const
{
    auto spans = transitions_;
    std::sort(spans.begin(), spans.end());
    std::vector<std::pair<double, double>> merged;
    for (const auto& span : spans)
        if (!merged.empty() && span.first <= merged.back().second)
            merged.back().second = std::max(merged.back().second, span.second);
        else
            merged.push_back(span);
    return merged;
}

void TimelineView::resized()
{
    // Check boxes under the deck name: Pitch and Vol in the first column, Hi/Mid/Low in the second.
    constexpr int kRow = 17;
    for (int d = 0; d < kMaxDecks; ++d)
    {
        const bool visible = d < numDecks();
        const auto header = laneBounds(d).withWidth(kHeaderWidth).reduced(6, 0).withTrimmedTop(26);
        const int column = header.getWidth() / 2;
        for (int p = 0; p < kNumParams; ++p)
        {
            auto& toggle = toggles_[static_cast<size_t>(d)][static_cast<size_t>(p)];
            toggle.setVisible(visible);
            const int col = p < 2 ? 0 : 1, row = p < 2 ? p : p - 2;
            toggle.setBounds(header.getX() + col * column, header.getY() + row * kRow, column, kRow);
        }
    }
}

// --- State ------------------------------------------------------------------

void TimelineView::refresh()
{
    const Set& set = document_.set();

    timings_.clear();
    for (const auto& clip : set.clips)
        if (const Track* track = set.findTrack(clip.trackId))
            try
            {
                timings_.emplace(clip.id, ClipTiming(clip, *track));
            }
            catch (const std::invalid_argument&)
            {
            }

    // A clip shows its worst issue.
    issues_.clear();
    for (const auto& issue : validate(set))
        for (Id id : issue.clipIds)
            if (issue.severity == Issue::Severity::Error || issues_.count(id) == 0)
                issues_[id] = issue.severity;

    transitions_.clear();
    for (const auto& a : set.clips)
        for (const auto& b : set.clips)
        {
            if (a.id >= b.id || a.deck == b.deck || timings_.count(a.id) == 0 || timings_.count(b.id) == 0)
                continue;
            const auto& ta = timings_.at(a.id);
            const auto& tb = timings_.at(b.id);
            const double from = std::max(ta.startSeconds(), tb.startSeconds());
            const double to = std::min(ta.endSeconds(), tb.endSeconds());
            if (from < to)
                transitions_.emplace_back(from, to);
        }

    if (selected_ != 0 && set.findClip(selected_) == nullptr)
        setSelectedClip(0);
    resized(); // the number of decks may have changed
    repaint();
}

void TimelineView::setSelectedClip(Id id)
{
    if (id == selected_)
        return;
    selected_ = id;
    if (onSelectionChanged)
        onSelectionChanged(id);
    repaint();
}

void TimelineView::updatePlayhead()
{
    const double now = engine_.playheadSeconds();
    if (std::abs(now - playhead_) < 1.0e-9)
        return;
    playhead_ = now;

    // Following keeps the playhead in the centre and scrolls the tracks past it. After scrolling
    // away, following resumes when the playhead reaches the centre of the view being looked at,
    // so the hand-back doesn't jump.
    const double span = laneWidth() / pixelsPerSecond_;
    const double centre = viewStart_ + span / 2.0;
    if (followSuspended_ && playhead_ >= centre && playhead_ <= viewStart_ + span)
        followSuspended_ = false;
    if (following() && drag_ == Drag::None)
        viewStart_ = playhead_ - span / 2.0;
    repaint();
}

// --- Geometry ---------------------------------------------------------------

juce::Rectangle<int> TimelineView::laneBounds(int deck) const
{
    const int n = numDecks();
    const int top = kRulerHeight;
    const int height = (getHeight() - top) / std::max(n, 1);
    return {0, top + deck * height, getWidth(), height};
}

juce::Rectangle<int> TimelineView::waveformBounds(int deck) const
{
    return laneBounds(deck).reduced(0, 4);
}

juce::Rectangle<int> TimelineView::automationBounds(int deck) const
{
    if (!anyAutomationShown(deck))
        return {};
    return waveformBounds(deck).withTrimmedTop(kTitleHeight);
}

int TimelineView::deckAt(int y) const
{
    for (int d = 0; d < numDecks(); ++d)
        if (laneBounds(d).contains(laneBounds(d).getX(), y))
            return d;
    return -1;
}

TimelineView::Hit TimelineView::clipAt(juce::Point<float> p) const
{
    const int deck = deckAt(static_cast<int>(p.y));
    if (deck < 0 || p.x < kHeaderWidth)
        return {};

    // Later clips are drawn on top, so search them first.
    const auto& clips = document_.set().clips;
    for (auto it = clips.rbegin(); it != clips.rend(); ++it)
    {
        if (it->deck != deck || timings_.count(it->id) == 0)
            continue;
        const auto& timing = timings_.at(it->id);
        const double x0 = timeToX(timing.startSeconds()), x1 = timeToX(timing.endSeconds());
        if (p.x < x0 - kEdgeGrab / 2.0 || p.x > x1 + kEdgeGrab / 2.0)
            continue;
        // Narrow clips: prefer moving over trimming.
        const double grab = std::min<double>(kEdgeGrab, (x1 - x0) / 4.0);
        if (p.x <= x0 + grab)
            return {it->id, Part::StartEdge};
        if (p.x >= x1 - grab)
            return {it->id, Part::EndEdge};
        return {it->id, Part::Body};
    }
    return {};
}

// --- Zoom -------------------------------------------------------------------

void TimelineView::setView(double start, double pixelsPerSecond)
{
    pixelsPerSecond_ = std::clamp(pixelsPerSecond, kMinPixelsPerSecond, kMaxPixelsPerSecond);
    viewStart_ = start;
    repaint();
}

void TimelineView::zoomAround(double factor, double anchorTime)
{
    const double x = timeToX(anchorTime);
    const double pps = std::clamp(pixelsPerSecond_ * factor, kMinPixelsPerSecond, kMaxPixelsPerSecond);
    setView(anchorTime - (x - kHeaderWidth) / pps, pps);
}

void TimelineView::zoomToRange(double from, double to)
{
    const double span = std::max(to - from, 0.01);
    setView(from - span * 0.05, laneWidth() / (span * 1.1));
}

void TimelineView::showPlayhead()
{
    followSuspended_ = false;
    const double span = laneWidth() / pixelsPerSecond_;
    setView(playhead_ - span / 2.0, pixelsPerSecond_);
}

void TimelineView::zoomIn()
{
    const double span = laneWidth() / pixelsPerSecond_;
    const bool playheadVisible = playhead_ >= viewStart_ && playhead_ <= viewStart_ + span;
    zoomAround(2.0, following() || playheadVisible ? playhead_ : viewStart_ + span / 2.0);
}

void TimelineView::zoomOut()
{
    const double span = laneWidth() / pixelsPerSecond_;
    zoomAround(0.5, following() ? playhead_ : viewStart_ + span / 2.0);
}

void TimelineView::zoomToSet()
{
    navigatedByHand();
    if (timings_.empty())
    {
        zoomToRange(0.0, 600.0);
        return;
    }
    double from = std::numeric_limits<double>::max(), to = std::numeric_limits<double>::lowest();
    for (const auto& [id, timing] : timings_)
    {
        from = std::min(from, timing.startSeconds());
        to = std::max(to, timing.endSeconds());
    }
    zoomToRange(from, to);
}

void TimelineView::zoomToClip()
{
    navigatedByHand();
    auto it = timings_.find(selected_);
    if (it == timings_.end())
        zoomToSet();
    else
        zoomToRange(it->second.startSeconds(), it->second.endSeconds());
}

void TimelineView::zoomToTransition()
{
    navigatedByHand();
    // The mix into the selected clip, or failing that the mix out of it.
    const Clip* clip = document_.set().findClip(selected_);
    auto it = timings_.find(selected_);
    if (clip == nullptr || it == timings_.end())
    {
        zoomToClip();
        return;
    }
    const double start = it->second.startSeconds(), end = it->second.endSeconds();

    std::optional<std::pair<double, double>> in, out;
    for (const auto& other : document_.set().clips)
    {
        auto ot = timings_.find(other.id);
        if (other.deck == clip->deck || ot == timings_.end())
            continue;
        const double os = ot->second.startSeconds(), oe = ot->second.endSeconds();
        if (os < start && oe > start)
            in = {start, std::min(oe, end)};
        else if (os > start && os < end && !out)
            out = {os, std::min(oe, end)};
    }

    const auto span = in ? in : out;
    if (!span)
    {
        zoomToClip();
        return;
    }
    const double centre = (span->first + span->second) / 2.0;
    const double half = std::max(span->second - span->first, kMinTransitionSpan) / 2.0;
    zoomToRange(centre - half, centre + half);
}

void TimelineView::zoomToBeats()
{
    // The selected clip if it's playing at the playhead, otherwise any clip that is.
    const Set& set = document_.set();
    const Clip* chosen = nullptr;
    for (const auto& clip : set.clips)
    {
        auto it = timings_.find(clip.id);
        if (it == timings_.end() || playhead_ < it->second.startSeconds() || playhead_ >= it->second.endSeconds())
            continue;
        if (chosen == nullptr || clip.id == selected_ || (chosen->id != selected_ && clip.deck < chosen->deck))
            chosen = &clip;
    }

    double span = 4.0;
    if (chosen != nullptr)
        if (const Track* track = set.findTrack(chosen->trackId))
            if (const auto grid = pool_.gridFor(*track))
                span = kBeatsInBeatZoom * grid->periodSeconds() / timings_.at(chosen->id).speedAt(playhead_);
    zoomToRange(playhead_ - span / 2.0, playhead_ + span / 2.0);
}

Id TimelineView::clipOnDeckNear(int deck, double seconds) const
{
    Id playing = 0, next = 0, previous = 0;
    double nextStart = std::numeric_limits<double>::max(), previousEnd = std::numeric_limits<double>::lowest();
    for (const auto& clip : document_.set().clips)
    {
        auto it = timings_.find(clip.id);
        if (clip.deck != deck || it == timings_.end())
            continue;
        const double start = it->second.startSeconds(), end = it->second.endSeconds();
        if (seconds >= start && seconds < end)
            playing = clip.id;
        else if (start >= seconds && start < nextStart)
        {
            next = clip.id;
            nextStart = start;
        }
        else if (end <= seconds && end > previousEnd)
        {
            previous = clip.id;
            previousEnd = end;
        }
    }
    return playing != 0 ? playing : next != 0 ? next : previous;
}

std::vector<Id> TimelineView::clipsInOrder() const
{
    std::vector<std::pair<double, Id>> starts;
    for (const auto& [id, timing] : timings_)
        starts.emplace_back(timing.startSeconds(), id);
    std::sort(starts.begin(), starts.end());
    std::vector<Id> ids;
    for (const auto& item : starts)
        ids.push_back(item.second);
    return ids;
}

const ClipTiming* TimelineView::timingOf(Id clip) const
{
    auto it = timings_.find(clip);
    return it == timings_.end() ? nullptr : &it->second;
}

// --- Painting ---------------------------------------------------------------

void TimelineView::paint(juce::Graphics& g)
{
    g.fillAll(kBackground);
    const Set& set = document_.set();

    for (int d = 0; d < numDecks(); ++d)
    {
        const auto lane = laneBounds(d);
        g.setColour(kLaneBackground);
        g.fillRect(lane.reduced(0, 1));
        g.setColour(deckColour(d));
        g.fillRect(lane.getX(), lane.getY() + 1, 4, lane.getHeight() - 2);
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
        g.drawText("Deck " + juce::String(d + 1), lane.withWidth(kHeaderWidth).withTrimmedLeft(10).withHeight(26),
                   juce::Justification::centredLeft);
    }

    {
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(kHeaderWidth, kRulerHeight, getWidth() - kHeaderWidth, getHeight() - kRulerHeight);
        for (const auto& clip : set.clips)
            if (auto it = timings_.find(clip.id); it != timings_.end())
                paintClip(g, clip, it->second);

        if (dropHint_)
        {
            const auto lane = laneBounds(dropHint_->first);
            const auto x = static_cast<float>(timeToX(dropHint_->second));
            g.setColour(juce::Colours::white);
            g.drawLine(x, static_cast<float>(lane.getY()), x, static_cast<float>(lane.getBottom()), 2.0f);
        }
    }

    paintRuler(g);

    const auto x = static_cast<float>(timeToX(playhead_));
    if (x >= kHeaderWidth)
    {
        g.setColour(juce::Colours::orange);
        g.drawLine(x, 0.0f, x, static_cast<float>(getHeight()), 1.5f);
    }
}

void TimelineView::paintRuler(juce::Graphics& g)
{
    const juce::Rectangle<int> ruler(kHeaderWidth, 0, getWidth() - kHeaderWidth, kRulerHeight);
    g.setColour(kRulerBackground);
    g.fillRect(ruler);

    juce::Graphics::ScopedSaveState state(g);
    g.reduceClipRegion(ruler);

    // Where two decks are audible together.
    g.setColour(juce::Colours::yellow.withAlpha(0.25f));
    for (const auto& [from, to] : transitions_)
        g.fillRect(juce::Rectangle<double>(timeToX(from), 0.0, (to - from) * pixelsPerSecond_, 6.0).toFloat());

    static constexpr double kSteps[] = {0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1,  0.2,  0.5,   1.0,   2.0,
                                        5.0,   10.0,  15.0,  30.0, 60.0, 120.0, 300.0, 600.0, 1200.0, 3600.0};
    double step = kSteps[std::size(kSteps) - 1];
    for (double s : kSteps)
        if (s * pixelsPerSecond_ >= 80.0)
        {
            step = s;
            break;
        }

    g.setFont(juce::FontOptions(11.0f));
    const double first = std::floor(xToTime(kHeaderWidth) / step) * step;
    const double last = xToTime(getWidth());
    for (double t = first; t <= last; t += step)
    {
        const auto x = static_cast<float>(timeToX(t));
        g.setColour(juce::Colours::grey);
        g.drawLine(x, kRulerHeight - 8.0f, x, static_cast<float>(kRulerHeight));
        g.setColour(juce::Colours::lightgrey);
        g.drawText(formatTime(std::abs(t) < step / 2.0 ? 0.0 : t, step < 1.0), static_cast<int>(x) + 3, 6, 90,
                   kRulerHeight - 8, juce::Justification::centredLeft);
    }
}

void TimelineView::paintClip(juce::Graphics& g, const Clip& clip, const ClipTiming& timing)
{
    const auto lane = waveformBounds(clip.deck);
    const double x0 = timeToX(timing.startSeconds()), x1 = timeToX(timing.endSeconds());
    if (x1 < kHeaderWidth || x0 > getWidth())
        return;

    // Clamp to the visible area so very deep zooms don't overflow int coordinates.
    const auto left = static_cast<int>(std::max(x0, kHeaderWidth - 10.0));
    const auto right = static_cast<int>(std::min(x1, getWidth() + 10.0));
    const juce::Rectangle<int> area(left, lane.getY(), std::max(1, right - left), lane.getHeight());

    const auto colour = deckColour(clip.deck);
    g.setColour(juce::Colour(0xff111317));
    g.fillRect(area);
    g.setColour(colour.withAlpha(0.35f));
    g.fillRect(area.withHeight(18));

    const Track* track = document_.set().findTrack(clip.trackId);
    const auto* entry = track != nullptr ? pool_.find(track->path) : nullptr;
    const auto waveArea = area.withTrimmedTop(18);
    juce::String status;
    if (entry == nullptr || entry->status == AudioPool::Status::Loading)
        status = "Loading...";
    else if (entry->status == AudioPool::Status::Failed)
        status = entry->error;
    else
    {
        paintWaveform(g, clip, timing, *entry->peaks, waveArea);
    }
    const auto grid = track != nullptr ? pool_.gridFor(*track) : std::nullopt;
    if (grid)
        paintBeats(g, clip, timing, *track, *grid, waveArea);

    if (anyAutomationShown(clip.deck))
    {
        // Dim the waveform a little so the lines read clearly over it.
        g.setColour(juce::Colours::black.withAlpha(0.25f));
        g.fillRect(waveArea);
        for (int p = 0; p < kNumParams; ++p)
            if (showsAutomation(clip.deck, static_cast<Param>(p)))
                paintAutomation(g, clip, timing, static_cast<Param>(p), waveArea);
    }

    juce::Colour border = colour.brighter(0.2f);
    float thickness = 1.0f;
    if (auto it = issues_.find(clip.id); it != issues_.end())
    {
        border = it->second == Issue::Severity::Error ? juce::Colours::red : juce::Colours::orange;
        thickness = 2.0f;
    }
    if (clip.id == selected_)
    {
        border = juce::Colours::white;
        thickness = 2.0f;
    }
    g.setColour(border);
    g.drawRect(area.toFloat(), thickness);

    const juce::String title = track != nullptr ? juce::String(juce::CharPointer_UTF8(track->title.c_str())) : "?";
    // Pitch, tempo and key as heard when the record comes in.
    const double speed = timing.speedAt(timing.startSeconds());
    juce::String text = title + "   " + juce::String::formatted("%+.2f%%", (speed - 1.0) * 100.0 + 0.0);
    if (grid)
        text << "   " << juce::String(grid->bpm * speed, 2) << " BPM";
    if (const auto key = track != nullptr ? pool_.keyFor(*track) : std::nullopt)
        text << "   " << pitchedKeyText(*key, speed, display_.keyNotation);
    if (status.isNotEmpty())
        text << "   " << status;
    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions(12.0f));
    const auto label = area.withHeight(18).withTrimmedLeft(std::max(0, kHeaderWidth - area.getX()) + 4);
    g.drawText(text, label, juce::Justification::centredLeft, true);
}

void TimelineView::paintWaveform(juce::Graphics& g, const Clip& clip, const ClipTiming& timing,
                                 const WaveformPeaks& peaks, juce::Rectangle<int> area)
{
    const int from = std::max(area.getX(), kHeaderWidth);
    const int to = std::min(area.getRight(), getWidth());
    const float mid = static_cast<float>(area.getCentreY());
    const float half = static_cast<float>(area.getHeight()) / 2.0f - 1.0f;
    const auto inFrame = static_cast<double>(clip.inFrame), outFrame = static_cast<double>(clip.outFrame);

    // Low behind, mids over it, highs on top, each mirrored about the centre line.
    auto bar = [&](float x, float level) {
        const float h = std::max(0.5f, std::min(level, 1.0f) * half);
        g.fillRect(x, mid - h, 1.0f, 2.0f * h);
    };
    for (int x = from; x < to; ++x)
    {
        const double f0 = std::clamp(timing.sourceFrameAt(xToTime(x)), inFrame, outFrame);
        const double f1 = std::clamp(timing.sourceFrameAt(xToTime(x + 1)), inFrame, outFrame);
        const auto bands = peaks.range(static_cast<int64_t>(std::floor(f0)), static_cast<int64_t>(std::ceil(f1)));
        const auto fx = static_cast<float>(x);
        g.setColour(kLowColour);
        bar(fx, bands.low);
        g.setColour(kMidColour);
        bar(fx, bands.mid);
        g.setColour(kHighColour);
        bar(fx, bands.high);
    }
}

void TimelineView::paintBeats(juce::Graphics& g, const Clip& clip, const ClipTiming& timing, const Track& track,
                              const BeatGrid& grid, juce::Rectangle<int> area)
{
    const double left = std::max(xToTime(std::max(area.getX(), kHeaderWidth)), timing.startSeconds());
    const double right = std::min(xToTime(std::min(area.getRight(), getWidth())), timing.endSeconds());
    if (right <= left)
        return;

    // Only once beats are far enough apart to judge phase by eye.
    const double period = grid.periodSeconds();
    if (period / timing.speedAt((left + right) / 2.0) * pixelsPerSecond_ < kMinBeatSpacingPixels)
        return;

    const double firstSource = timing.sourceFrameAt(left) / track.sampleRate;
    const double lastSource = timing.sourceFrameAt(right) / track.sampleRate;
    const auto firstBeat = static_cast<int64_t>(std::ceil((firstSource - grid.firstBeatSeconds) / period));
    const auto lastBeat = static_cast<int64_t>(std::floor((lastSource - grid.firstBeatSeconds) / period));
    for (int64_t k = firstBeat; k <= lastBeat; ++k)
    {
        const double frame = (grid.firstBeatSeconds + static_cast<double>(k) * period) * track.sampleRate;
        if (frame < static_cast<double>(clip.inFrame) || frame > static_cast<double>(clip.outFrame))
            continue;
        const auto x = static_cast<float>(timeToX(timing.timelineSecondsAt(frame)));
        // Bar lines red, every 4th bar green, every 16th bar blue.
        auto every = [k](int64_t beats) { return ((k % beats) + beats) % beats == 0; };
        const bool downbeat = every(kBeatsPerBar);
        g.setColour(every(kBeatsPerPhrase)       ? juce::Colour(0xff3d8bff)
                    : every(4 * kBeatsPerBar) ? juce::Colour(0xff35d05a)
                    : downbeat  ? juce::Colours::red.withAlpha(0.9f)
                                : juce::Colours::white.withAlpha(0.45f));
        g.drawLine(x, static_cast<float>(area.getY()), x, static_cast<float>(area.getBottom()), downbeat ? 2.0f : 1.0f);
    }
}


std::vector<juce::Point<float>> TimelineView::automationVertices(const Clip& clip, const ClipTiming& timing,
                                                                 Param param, juce::Rectangle<int> area) const
{
    // Straight lines from the starting point through the keypoints, then level to the end.
    // (Pitch slides are linear in time, so on the timeline they are exactly straight.)
    auto xOf = [&](double frame) { return static_cast<float>(timeToX(timing.timelineSecondsAt(frame))); };
    auto yOf = [&](double value) { return static_cast<float>(valueToY(param, value, area)); };
    std::vector<juce::Point<float>> vertices{{xOf(static_cast<double>(clip.inFrame)), yOf(baseValue(clip, param))}};
    for (const auto& point : clip.points(param))
        if (point.sourceFrame >= clip.inFrame && point.sourceFrame <= clip.outFrame)
            vertices.push_back({xOf(static_cast<double>(point.sourceFrame)), yOf(point.value)});
    // A keypoint past the out point: carry its slide to the clip end.
    vertices.push_back({xOf(static_cast<double>(clip.outFrame)),
                        yOf(valueAt(clip, param, static_cast<double>(clip.outFrame)))});
    return vertices;
}

void TimelineView::paintAutomation(juce::Graphics& g, const Clip& clip, const ClipTiming& timing, Param param,
                                   juce::Rectangle<int> area)
{
    const auto vertices = automationVertices(clip, timing, param, area);
    juce::Path line;
    line.startNewSubPath(vertices.front());
    for (size_t i = 1; i < vertices.size(); ++i)
        line.lineTo(vertices[i]);
    const auto colour = paramColour(param);
    g.setColour(juce::Colours::black.withAlpha(0.6f));
    g.strokePath(line, juce::PathStrokeType(3.5f)); // outline, so the line shows over any waveform colour
    g.setColour(colour);
    g.strokePath(line, juce::PathStrokeType(1.75f));

    // The starting point is a square, keypoints are circles.
    const auto start = vertices.front();
    g.fillRect(start.x - 4.0f, start.y - 4.0f, 8.0f, 8.0f);
    for (size_t i = 1; i + 1 < vertices.size(); ++i)
        if (vertices[i].x > static_cast<float>(kHeaderWidth - 5) && vertices[i].x < static_cast<float>(getWidth() + 5))
        {
            g.setColour(juce::Colours::black);
            g.fillEllipse(vertices[i].x - 4.5f, vertices[i].y - 4.5f, 9.0f, 9.0f);
            g.setColour(colour);
            g.fillEllipse(vertices[i].x - 3.5f, vertices[i].y - 3.5f, 7.0f, 7.0f);
        }
}

Id TimelineView::clipAtTime(int deck, double x) const
{
    const double t = xToTime(x);
    for (const auto& clip : document_.set().clips)
        if (auto it = timings_.find(clip.id);
            clip.deck == deck && it != timings_.end() && t >= it->second.startSeconds() && t <= it->second.endSeconds())
            return clip.id;
    return 0;
}

std::optional<TimelineView::AutomationHit> TimelineView::automationAt(juce::Point<float> position) const
{
    const int deck = deckAt(static_cast<int>(position.y));
    if (deck < 0 || !automationBounds(deck).contains(position.toInt()))
        return std::nullopt;
    const Id id = clipAtTime(deck, position.x);
    const Clip* clip = document_.set().findClip(id);
    const auto timing = timings_.find(id);
    if (clip == nullptr || timing == timings_.end())
        return std::nullopt;
    const auto area = automationBounds(deck);

    // The nearest keypoint wins; failing that, the nearest line.
    std::optional<AutomationHit> best;
    float bestDistance = std::numeric_limits<float>::max();
    for (int p = 0; p < kNumParams; ++p)
    {
        const auto param = static_cast<Param>(p);
        if (!showsAutomation(deck, param))
            continue;
        const auto vertices = automationVertices(*clip, timing->second, param, area);

        // Vertex i is the starting point (i == 0) or the i-th keypoint at or after the needle drop.
        std::vector<int> indexOf{kStartPoint};
        const auto& points = clip->points(param);
        for (size_t i = 0; i < points.size(); ++i)
            if (points[i].sourceFrame >= clip->inFrame && points[i].sourceFrame <= clip->outFrame)
                indexOf.push_back(static_cast<int>(i));
        for (size_t v = 0; v < indexOf.size(); ++v)
        {
            const float distance = vertices[v].getDistanceFrom(position) - 1000.0f; // keypoints first
            if (distance + 1000.0f <= kKeypointGrab && distance < bestDistance)
            {
                bestDistance = distance;
                best = AutomationHit{id, param, indexOf[v], 0, 0.0};
            }
        }

        for (size_t v = 0; v + 1 < vertices.size(); ++v)
        {
            const auto a = vertices[v], b = vertices[v + 1];
            if (position.x < a.x || position.x > b.x || b.x <= a.x)
                continue;
            const float y = a.y + (b.y - a.y) * (position.x - a.x) / (b.x - a.x);
            const float distance = std::abs(position.y - y);
            if (distance <= kLineGrab && distance < bestDistance)
            {
                const double t = xToTime(position.x);
                const double frame = std::clamp(timing->second.sourceFrameAt(t), static_cast<double>(clip->inFrame),
                                                static_cast<double>(clip->outFrame));
                const double value = param == Param::Pitch ? pitchStepsFromSpeed(timing->second.speedAt(t))
                                                           : valueAt(*clip, param, frame);
                bestDistance = distance;
                best = AutomationHit{id, param, std::nullopt, static_cast<int64_t>(std::llround(frame)), value};
            }
        }
    }
    return best;
}

double TimelineView::valueToY(Param param, double value, juce::Rectangle<int> area) const
{
    const auto range = paramRange(param);
    const double fraction = (value - range.min) / (range.max - range.min);
    return area.getBottom() - 3.0 - fraction * (area.getHeight() - 6.0);
}

double TimelineView::yToValue(Param param, double y, juce::Rectangle<int> area) const
{
    const auto range = paramRange(param);
    const double fraction = std::clamp((area.getBottom() - 3.0 - y) / (area.getHeight() - 6.0), 0.0, 1.0);
    return range.min + fraction * (range.max - range.min);
}

void TimelineView::automationMouseDown(const juce::MouseEvent& e, const AutomationHit& hit)
{
    setSelectedClip(hit.clip);
    if (e.mods.isPopupMenu())
    {
        showAutomationMenu(hit.clip, hit.param, hit.keypoint);
        return;
    }

    const Clip& clip = *document_.set().findClip(hit.clip);
    dragTiming_.emplace(timings_.at(hit.clip));
    dragOrigin_ = clip;
    dragParam_ = hit.param;
    document_.beginGesture();
    if (hit.keypoint)
        dragKeypoint_ = *hit.keypoint;
    else
    {
        // A new keypoint on the line where clicked, at the line's value, then drag it.
        const Id id = hit.clip;
        document_.edit([&](Set& s) { return edit::setAutomationPoint(s, id, hit.param, hit.frame, hit.value); });
        const auto& points = document_.set().findClip(id)->points(hit.param);
        dragKeypoint_ = static_cast<int>(
            std::find_if(points.begin(), points.end(), [&](const auto& p) { return p.sourceFrame == hit.frame; }) -
            points.begin());
    }
    drag_ = Drag::Keypoint;
}

void TimelineView::showAutomationMenu(Id clip, Param param, std::optional<int> keypoint)
{
    juce::PopupMenu menu;
    menu.addItem(1, "Delete keypoint", keypoint.has_value() && *keypoint != kStartPoint);
    menu.addItem(2, juce::String("Clear ") + paramName(param) + " automation",
                 !document_.set().findClip(clip)->points(param).empty());
    menu.showMenuAsync(juce::PopupMenu::Options(), [safe = juce::Component::SafePointer(this), clip, param,
                                                    keypoint](int result) {
        if (safe == nullptr)
            return;
        if (result == 1 && keypoint && *keypoint >= 0)
            safe->document_.edit([&](Set& s) {
                return edit::deleteAutomationPoint(s, clip, param, static_cast<size_t>(*keypoint));
            });
        else if (result == 2)
            safe->document_.edit([&](Set& s) { return edit::clearAutomation(s, clip, param); });
    });
}

// --- Mouse ------------------------------------------------------------------

void TimelineView::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();

    if (e.y < kRulerHeight && e.x >= kHeaderWidth)
    {
        drag_ = Drag::Scrub;
        engine_.seek(std::max(0.0, xToTime(e.x)));
        return;
    }

    if (const auto hit = automationAt(e.position))
    {
        automationMouseDown(e, *hit);
        return;
    }

    const Hit hit = clipAt(e.position);
    if (hit.clip == 0)
    {
        setSelectedClip(0);
        if (e.x >= kHeaderWidth)
            engine_.seek(std::max(0.0, xToTime(e.x)));
        return;
    }

    setSelectedClip(hit.clip);
    dragOrigin_ = *document_.set().findClip(hit.clip);
    dragAnchorTime_ = xToTime(e.x);
    drag_ = hit.part == Part::StartEdge ? Drag::TrimStart : hit.part == Part::EndEdge ? Drag::TrimEnd : Drag::Move;
    document_.beginGesture();
}

void TimelineView::mouseDrag(const juce::MouseEvent& e)
{
    const double t = xToTime(e.x);
    const Id id = dragOrigin_.id;
    switch (drag_)
    {
    case Drag::Scrub:
        engine_.seek(std::max(0.0, t));
        break;
    case Drag::Move:
    {
        // Shift drags at a tenth of the speed for fine alignment.
        const double delta = (t - dragAnchorTime_) * (e.mods.isShiftDown() ? kFineDragFactor : 1.0);
        const int deck = deckAt(e.y);
        document_.edit([&](Set& s) { return edit::moveClip(s, id, dragOrigin_.startSeconds + delta,
                                                           deck >= 0 ? deck : dragOrigin_.deck); });
        break;
    }
    case Drag::TrimStart:
        document_.edit([&](Set& s) { return edit::trimClipStart(s, id, t); });
        break;
    case Drag::TrimEnd:
        document_.edit([&](Set& s) { return edit::trimClipEnd(s, id, t); });
        break;
    case Drag::Keypoint:
    {
        // Positions come from the clip's timing at the start of the drag: dragging a pitch
        // keypoint changes the timing, and following it would make the point run away.
        const auto frame = static_cast<int64_t>(std::llround(std::clamp(
            dragTiming_->sourceFrameAt(t), static_cast<double>(dragOrigin_.inFrame), static_cast<double>(dragOrigin_.outFrame))));
        const double value = yToValue(dragParam_, e.position.y, automationBounds(dragOrigin_.deck));
        const auto param = dragParam_;
        const auto index = dragKeypoint_;
        document_.edit([&](Set& s) {
            return index == kStartPoint ? edit::setStartValue(s, id, param, value) // up and down only
                                        : edit::moveAutomationPoint(s, id, param, static_cast<size_t>(index), frame, value);
        });
        break;
    }
    case Drag::None:
        break;
    }
}

void TimelineView::mouseUp(const juce::MouseEvent& e)
{
    if (drag_ == Drag::Move || drag_ == Drag::TrimStart || drag_ == Drag::TrimEnd || drag_ == Drag::Keypoint)
        document_.endGesture();
    // Shift-click without dragging sets the downbeat (shift-drag is a fine move).
    if (drag_ == Drag::Move && e.mods.isShiftDown() && !e.mouseWasDraggedSinceMouseDown())
        setDownbeatAt(dragOrigin_.id, e.mouseDownPosition.x, !e.mods.isAltDown());
    drag_ = Drag::None;
    dragTiming_.reset();
}

void TimelineView::setDownbeatAt(Id clipId, float x, bool snapToBeatLine)
{
    constexpr float kSnapPixels = 5.0f;
    const Set& set = document_.set();
    const Clip* clip = set.findClip(clipId);
    const Track* track = clip != nullptr ? set.findTrack(clip->trackId) : nullptr;
    const auto timing = timings_.find(clipId);
    const auto grid = track != nullptr ? pool_.gridFor(*track) : std::nullopt;
    if (!grid || timing == timings_.end())
        return; // no tempo to build a grid from; the inspector asks for a BPM

    double beat = timing->second.sourceFrameAt(xToTime(x)) / track->sampleRate;
    if (snapToBeatLine)
    {
        const double period = grid->periodSeconds();
        const double nearest = grid->firstBeatSeconds + std::round((beat - grid->firstBeatSeconds) / period) * period;
        const double nearestX = timeToX(timing->second.timelineSecondsAt(nearest * track->sampleRate));
        if (std::abs(nearestX - x) <= kSnapPixels)
            beat = nearest;
    }
    const BeatGrid updated{grid->bpm, beat};
    const Id trackId = track->id;
    document_.edit([&](Set& s) { return edit::setTrackGrid(s, trackId, updated); });
}

void TimelineView::mouseMove(const juce::MouseEvent& e)
{
    if (const auto hit = automationAt(e.position))
    {
        setMouseCursor(hit->keypoint ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::CrosshairCursor);
        return;
    }
    const Hit hit = clipAt(e.position);
    setMouseCursor(hit.part == Part::StartEdge || hit.part == Part::EndEdge ? juce::MouseCursor::LeftRightResizeCursor
                                                                            : juce::MouseCursor::NormalCursor);
}

void TimelineView::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (const auto hit = automationAt(e.position); hit && hit->keypoint && *hit->keypoint >= 0)
    {
        const auto index = static_cast<size_t>(*hit->keypoint);
        document_.edit([&](Set& s) { return edit::deleteAutomationPoint(s, hit->clip, hit->param, index); });
        return;
    }
    if (clipAt(e.position).clip != 0)
        zoomToClip();
}

void TimelineView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        // While following, zoom around the playhead and keep following.
        zoomAround(std::pow(2.0, wheel.deltaY * 4.0), following() ? playhead_ : xToTime(e.x));
        return;
    }
    // Lanes fit the window, so both scroll directions pan through time.
    navigatedByHand();
    const double pixels = (wheel.deltaX + wheel.deltaY) * 300.0;
    setView(viewStart_ - pixels / pixelsPerSecond_, pixelsPerSecond_);
}

void TimelineView::mouseMagnify(const juce::MouseEvent& e, float scaleFactor)
{
    zoomAround(scaleFactor, following() ? playhead_ : xToTime(e.x));
}

// --- Drag and drop ----------------------------------------------------------

void TimelineView::dropFiles(const juce::Array<juce::File>& files, int x, int y)
{
    dropHint_.reset();
    repaint();
    const int deck = deckAt(y);
    if (deck < 0 || !onFileDropped)
        return;

    // Several files go one after another on the same deck, with time to change records.
    double start = std::max(0.0, xToTime(x));
    for (const auto& file : files)
        if (const auto end = onFileDropped(file, deck, start))
            start = *end + kMinChangeoverSeconds;
}

bool TimelineView::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& path : files)
        if (pool_.canLoad(juce::File(path)))
            return true;
    return false;
}

void TimelineView::fileDragMove(const juce::StringArray&, int x, int y)
{
    const int deck = deckAt(y);
    dropHint_ = deck >= 0 ? std::optional(std::pair{deck, xToTime(x)}) : std::nullopt;
    repaint();
}

void TimelineView::fileDragExit(const juce::StringArray&)
{
    dropHint_.reset();
    repaint();
}

void TimelineView::filesDropped(const juce::StringArray& paths, int x, int y)
{
    juce::Array<juce::File> files;
    for (const auto& path : paths)
        if (pool_.canLoad(juce::File(path)))
            files.add(juce::File(path));
    dropFiles(files, x, y);
}

bool TimelineView::isInterestedInDragSource(const SourceDetails& details)
{
    return dynamic_cast<juce::FileTreeComponent*>(details.sourceComponent.get()) != nullptr;
}

void TimelineView::itemDragMove(const SourceDetails& details)
{
    fileDragMove({}, details.localPosition.x, details.localPosition.y);
}

void TimelineView::itemDragExit(const SourceDetails&)
{
    fileDragExit({});
}

void TimelineView::itemDropped(const SourceDetails& details)
{
    auto* tree = dynamic_cast<juce::FileTreeComponent*>(details.sourceComponent.get());
    if (tree == nullptr)
        return;
    juce::Array<juce::File> files;
    for (int i = 0; i < tree->getNumSelectedFiles(); ++i)
        if (pool_.canLoad(tree->getSelectedFile(i)))
            files.add(tree->getSelectedFile(i));
    dropFiles(files, details.localPosition.x, details.localPosition.y);
}

} // namespace vmp

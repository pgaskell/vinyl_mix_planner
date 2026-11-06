#include "app/TimelineView.h"

#include "app/DeckColours.h"
#include "app/LibraryPanel.h"
#include "app/TimeFormat.h"
#include "core/Edits.h"
#include "engine/MixerChannel.h"
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
// Closer than this, lines are just noise. Zooming out drops beats first, then bar lines, then
// 4-bar lines, so the 16-bar lines still show the song's structure.
constexpr double kMinBeatSpacingPixels = 8.0;
constexpr int kBeatsInBeatZoom = 8;
constexpr float kKeypointGrab = 6.0f;
constexpr int kTitleHeight = 18;
constexpr float kLineGrab = 5.0f;

// Nudges: pushes (the record sped up, its music earlier) and platter drags (held back).
const juce::Colour kPushColour{0xff35d6e8};
const juce::Colour kDragColour{0xffb07cff};
constexpr int kNudgeStrip = 14; // the deck action lane, along the bottom of each clip
constexpr int kPinHandle = 9; // a beat pin's handle, at the top of the waveform
constexpr float kBeatGrabPixels = 5.0f;
const juce::Colour kPinColour(0xffffc04d);
const juce::Colour kLinkColour(0xff6cf08a);
// While a hand is on a record, its touch is written this far ahead of what's heard.
constexpr double kTouchLeadSeconds = 0.25;
// A press on a playing record takes hold of it after this long, or once moved this far; a quicker
// click only selects it.
constexpr double kTouchDelaySeconds = 0.15;
constexpr int kTouchDragPixels = 4;

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
    case Param::Color:
        return juce::Colour(0xffff7ac8);
    }
    return juce::Colours::white;
}

// CDJ-3000 style 3-band colours.
const juce::Colour kLowColour{0xff2a6cf0};
const juce::Colour kMidColour{0xffffa31a};
const juce::Colour kHighColour{0xfff4f4f4};
// The trim that draws a full-scale record to the lane's edge (4 dB above the centre), so the
// waveform's height shows the record's level after its trim: louder ones fill the lane and
// flatten at the edge, quieter ones shrink. At the centre a record peaks at 63% of the height.
constexpr double kWaveformFullTrim = 0.75;

const juce::Colour kBackground{0xff1b1d21};
const juce::Colour kLaneBackground{0xff23262b};
const juce::Colour kRulerBackground{0xff2c3036};
} // namespace

TimelineView::TimelineView(SetDocument& document, AudioPool& pool, Engine& engine, const DisplayOptions& display)
    : document_(document), pool_(pool), engine_(engine), display_(display)
{
    setWantsKeyboardFocus(true);
    static const char* const kShortNames[] = {"Pitch", "Vol", "Hi", "Mid", "Low", "Color"};
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
    static const char* const kHeightNames[] = {"S", "M", "L", "Full"};
    for (size_t h = 0; h < laneHeightButtons_.size(); ++h)
    {
        auto& button = laneHeightButtons_[h];
        button.setButtonText(kHeightNames[h]);
        button.setTooltip("Lane height: small, medium, large or full, for all decks (H)");
        button.setWantsKeyboardFocus(false);
        button.setClickingTogglesState(false);
        button.setConnectedEdges((h > 0 ? juce::Button::ConnectedOnLeft : 0) |
                                 (h + 1 < laneHeightButtons_.size() ? juce::Button::ConnectedOnRight : 0));
        button.onClick = [this, h] {
            if (onLaneHeightChosen)
                onLaneHeightChosen(static_cast<LaneHeight>(h));
        };
        addAndMakeVisible(button);
    }
    laneHeightButtons_.back().setToggleState(true, juce::dontSendNotification);
    for (int d = 0; d < kMaxDecks; ++d)
    {
        pitchFaders_[static_cast<size_t>(d)] =
            std::make_unique<PitchFader>(d, document_, [this] { return engine_.playheadSeconds(); });
        addChildComponent(*pitchFaders_[static_cast<size_t>(d)]);
    }
}

void TimelineView::updatePitchFaders(bool playing)
{
    for (int d = 0; d < numDecks(); ++d)
        pitchFaders_[static_cast<size_t>(d)]->show(clipForDeck(document_, d, selected_, playhead_, playing), playhead_);
}

void TimelineView::setLaneHeight(LaneHeight height)
{
    laneHeight_ = height;
    for (size_t h = 0; h < laneHeightButtons_.size(); ++h)
        laneHeightButtons_[h].setToggleState(static_cast<size_t>(height) == h, juce::dontSendNotification);
    resized();
    repaint();
}

void TimelineView::setShownAutomation(const std::array<std::array<bool, kNumParams>, kMaxDecks>& shown)
{
    for (int d = 0; d < kMaxDecks; ++d)
        for (int p = 0; p < kNumParams; ++p)
            setShowsAutomation(d, static_cast<Param>(p), shown[static_cast<size_t>(d)][static_cast<size_t>(p)]);
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
    // The pitch fader down the left of the header; check boxes under the deck name, if the lane is
    // tall enough: Pitch, Vol and Color in the first column, Hi/Mid/Low in the second.
    constexpr int kRow = 17;
    auto corner = juce::Rectangle<int>(0, 0, kHeaderWidth, kRulerHeight).reduced(6, 3);
    const int buttonWidth = corner.getWidth() / static_cast<int>(laneHeightButtons_.size());
    for (auto& button : laneHeightButtons_)
        button.setBounds(corner.removeFromLeft(buttonWidth));
    for (int d = 0; d < kMaxDecks; ++d)
    {
        const auto lane = laneBounds(d);
        pitchFaders_[static_cast<size_t>(d)]->setVisible(d < numDecks());
        pitchFaders_[static_cast<size_t>(d)]->setBounds(lane.withX(6).withWidth(kPitchWidth).reduced(0, 3));
        const bool visible = d < numDecks() && lane.getHeight() >= 26 + 3 * kRow + 4;
        const auto header = lane.withTrimmedLeft(6 + kPitchWidth).withWidth(kHeaderWidth - 6 - kPitchWidth)
                                .reduced(6, 0)
                                .withTrimmedTop(26);
        const int column = header.getWidth() / 2;
        for (int p = 0; p < kNumParams; ++p)
        {
            auto& toggle = toggles_[static_cast<size_t>(d)][static_cast<size_t>(p)];
            toggle.setVisible(visible);
            const bool first = p < 2 || static_cast<Param>(p) == Param::Color;
            const int col = first ? 0 : 1, row = p < 2 ? p : first ? 2 : p - 2;
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
    // A hand on a playing record: it takes hold once pressed a moment or moved (a quick click only
    // selects), then follows the mouse.
    if (drag_ == Drag::Touch && engine_.isPlaying())
    {
        if (touch_.active)
            sampleTouch();
        else if (juce::Time::getMillisecondCounterHiRes() / 1000.0 - touch_.pressClock >= kTouchDelaySeconds)
            startTouch();
    }
    // The engine reports its position once per audio block, which is jerky on screen. While
    // playing, carry it forward by the time since the last report.
    const double reported = engine_.playheadSeconds();
    const double clock = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    if (std::abs(reported - lastReported_) > 1.0e-9)
    {
        lastReported_ = reported;
        lastReportClock_ = clock;
    }
    constexpr double kMaxCarry = 0.05; // never run ahead of the engine by more than this
    const double now = engine_.isPlaying() ? reported + std::min(clock - lastReportClock_, kMaxCarry) : reported;
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
    const int full = (getHeight() - top) / std::max(n, 1);
    static constexpr double kFractions[] = {0.25, 0.5, 0.75, 1.0};
    const int height = std::min(full, std::max(kMinLaneHeight, static_cast<int>(full * kFractions[static_cast<int>(laneHeight_)])));
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
            {
                const auto& timing = timings_.at(chosen->id);
                const double source = timing.sourceFrameAt(playhead_) / track->sampleRate;
                span = kBeatsInBeatZoom * grid->periodAt(source) / timing.speedAt(playhead_);
            }
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
        g.setFont(laneFont_);
        g.drawText("Deck " + juce::String(d + 1),
                   lane.withWidth(kHeaderWidth).withTrimmedLeft(6 + kPitchWidth + 6).withHeight(26),
                   juce::Justification::centredLeft);
    }

    {
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(kHeaderWidth, kRulerHeight, getWidth() - kHeaderWidth, getHeight() - kRulerHeight);
        for (const auto& clip : set.clips)
            if (auto it = timings_.find(clip.id); it != timings_.end())
                paintClip(g, clip, it->second);
        paintBeatGuides(g);
        paintLinks(g);

        if (dropHint_)
        {
            const auto lane = laneBounds(dropHint_->first);
            const auto x = static_cast<float>(timeToX(dropHint_->second));
            g.setColour(juce::Colours::white);
            g.drawLine(x, static_cast<float>(lane.getY()), x, static_cast<float>(lane.getBottom()), 2.0f);
        }
    }

    paintRuler(g);
    if (cleanUp_)
        paintCleanUp(g);

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

    g.setFont(rulerFont_);
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

    // Glitches: small red triangles pointing down at the lanes.
    g.setColour(juce::Colour(0xffff3b30));
    const double firstVisible = xToTime(kHeaderWidth - 4), lastVisible = xToTime(getWidth() + 4);
    for (double t : glitchMarks_)
    {
        if (t < firstVisible || t > lastVisible)
            continue;
        const auto x = static_cast<float>(timeToX(t));
        juce::Path mark;
        mark.addTriangle(x - 4.0f, kRulerHeight - 7.0f, x + 4.0f, kRulerHeight - 7.0f, x, kRulerHeight);
        g.fillPath(mark);
    }
}

void TimelineView::setGlitchMarks(std::vector<double> seconds)
{
    glitchMarks_ = std::move(seconds);
    repaint(kHeaderWidth, 0, getWidth() - kHeaderWidth, kRulerHeight);
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

    paintActions(g, clip, timing, waveArea);

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
    g.setFont(titleFont_);
    const auto label = area.withHeight(18).withTrimmedLeft(std::max(0, kHeaderWidth - area.getX()) + 4);
    g.drawText(text, label, juce::Justification::centredLeft, true);
}

void TimelineView::paintWaveform(juce::Graphics& g, const Clip& clip, const ClipTiming& timing,
                                 const WaveformPeaks& peaks, juce::Rectangle<int> area)
{
    const double from = std::max(area.getX(), kHeaderWidth);
    const double to = std::min(area.getRight(), getWidth());
    const float mid = static_cast<float>(area.getCentreY());
    const float half = static_cast<float>(area.getHeight()) / 2.0f - 1.0f;
    const float level = MixerChannel::trimGain(clip.mixer.trim) / MixerChannel::trimGain(kWaveformFullTrim);
    const auto inFrame = static_cast<double>(clip.inFrame), outFrame = static_cast<double>(clip.outFrame);

    // Columns are a pixel wide but counted from the clip start, not the screen edge, so while the
    // view scrolls each column keeps covering the same audio and only its position moves. Sampling
    // from the screen edge made every column's peak change as it scrolled, which flickered.
    const double columnSeconds = 1.0 / pixelsPerSecond_;
    const double start = timing.startSeconds(), end = timing.endSeconds();
    const auto firstColumn = static_cast<int64_t>(std::floor((xToTime(from) - start) / columnSeconds));
    const auto lastColumn = static_cast<int64_t>(std::ceil((xToTime(to) - start) / columnSeconds));

    // One outline per band, mirrored about the centre and filled, so edges are anti-aliased and
    // move smoothly between pixels.
    std::array<juce::Path, 3> shapes;
    std::array<std::vector<juce::Point<float>>, 3> tops;
    for (auto& top : tops)
        top.reserve(static_cast<size_t>(lastColumn - firstColumn + 2));
    for (int64_t k = firstColumn; k <= lastColumn; ++k)
    {
        const double t0 = std::clamp(start + static_cast<double>(k) * columnSeconds, start, end);
        const double t1 = std::clamp(t0 + columnSeconds, start, end);
        const double f0 = std::clamp(timing.sourceFrameAt(t0), inFrame, outFrame);
        const double f1 = std::clamp(timing.sourceFrameAt(t1), inFrame, outFrame);
        const auto bands = peaks.range(static_cast<int64_t>(std::floor(f0)), static_cast<int64_t>(std::ceil(f1)));
        const auto x = static_cast<float>(timeToX((t0 + t1) / 2.0));
        const float levels[] = {bands.low, bands.mid, bands.high};
        for (size_t b = 0; b < 3; ++b)
            tops[b].push_back({x, mid - std::max(0.5f, std::min(levels[b] * level, 1.0f) * half)});
    }
    for (size_t b = 0; b < 3; ++b)
    {
        if (tops[b].empty())
            continue;
        shapes[b].startNewSubPath(tops[b].front());
        for (size_t i = 1; i < tops[b].size(); ++i)
            shapes[b].lineTo(tops[b][i]);
        for (auto it = tops[b].rbegin(); it != tops[b].rend(); ++it)
            shapes[b].lineTo(it->x, 2.0f * mid - it->y);
        shapes[b].closeSubPath();
    }

    // Low behind, mids over it, highs on top.
    g.setColour(kLowColour);
    g.fillPath(shapes[0]);
    g.setColour(kMidColour);
    g.fillPath(shapes[1]);
    g.setColour(kHighColour);
    g.fillPath(shapes[2]);
}

namespace
{
bool everyNth(int64_t beat, int64_t n)
{
    return ((beat % n) + n) % n == 0;
}

// Bar lines red, every 4th bar green, every 16th bar blue; other beats white.
juce::Colour beatColour(int64_t beat)
{
    return everyNth(beat, kBeatsPerPhrase)       ? juce::Colour(0xff3d8bff)
           : everyNth(beat, 4 * kBeatsPerBar) ? juce::Colour(0xff35d05a)
           : everyNth(beat, kBeatsPerBar)     ? juce::Colours::red
                                                : juce::Colours::white;
}
} // namespace

void TimelineView::forEachVisibleBeat(const Clip& clip, const ClipTiming& timing, const Track& track,
                                      const BeatGrid& grid, double left, double right, int64_t minimumStep,
                                      const std::function<void(float x, int64_t beat)>& draw) const
{
    left = std::max(left, timing.startSeconds());
    right = std::min(right, timing.endSeconds());
    if (right <= left)
        return;

    // The finest level whose lines are far enough apart: every beat, bar, 4 bars or 16 bars.
    const double middle = (left + right) / 2.0;
    const double period = grid.periodAt(timing.sourceFrameAt(middle) / track.sampleRate);
    const double beatPixels = period / timing.speedAt(middle) * pixelsPerSecond_;
    int64_t step = 0;
    for (int64_t s : {int64_t{1}, int64_t{kBeatsPerBar}, int64_t{4 * kBeatsPerBar}, int64_t{kBeatsPerPhrase}})
        if (step == 0 && beatPixels * static_cast<double>(s) >= kMinBeatSpacingPixels)
            step = s;
    if (step == 0)
        return; // even the 16-bar lines would crowd together
    step = std::max(step, minimumStep);

    const double firstSource = timing.sourceFrameAt(left) / track.sampleRate;
    const double lastSource = timing.sourceFrameAt(right) / track.sampleRate;
    auto firstBeat = static_cast<int64_t>(std::ceil(grid.beatAt(firstSource)));
    const auto lastBeat = static_cast<int64_t>(std::floor(grid.beatAt(lastSource)));
    firstBeat += ((step - firstBeat % step) % step + step) % step; // round up to a multiple of step
    for (int64_t k = firstBeat; k <= lastBeat; k += step)
    {
        const double frame = grid.secondsOfBeat(static_cast<double>(k)) * track.sampleRate;
        if (frame < static_cast<double>(clip.inFrame) || frame > static_cast<double>(clip.outFrame))
            continue;
        draw(static_cast<float>(timeToX(timing.timelineSecondsAt(frame))), k);
    }
}

void TimelineView::paintBeats(juce::Graphics& g, const Clip& clip, const ClipTiming& timing, const Track& track,
                              const BeatGrid& grid, juce::Rectangle<int> area)
{
    const double left = xToTime(std::max(area.getX(), kHeaderWidth));
    const double right = xToTime(std::min(area.getRight(), getWidth()));
    forEachVisibleBeat(clip, timing, track, grid, left, right, 1, [&](float x, int64_t beat) {
        const bool downbeat = everyNth(beat, kBeatsPerBar);
        g.setColour(beatColour(beat).withAlpha(downbeat ? 0.9f : 0.45f));
        g.drawLine(x, static_cast<float>(area.getY()), x, static_cast<float>(area.getBottom()), downbeat ? 2.0f : 1.0f);
    });
    // Pins: shown at any zoom, as a handle with a line through the waveform.
    for (const auto& pin : grid.pins)
    {
        const double frame = pin.seconds * track.sampleRate;
        if (frame < static_cast<double>(clip.inFrame) || frame > static_cast<double>(clip.outFrame))
            continue;
        const auto x = static_cast<float>(timeToX(timing.timelineSecondsAt(frame)));
        if (x < static_cast<float>(area.getX()) - kPinHandle || x > static_cast<float>(area.getRight()) + kPinHandle)
            continue;
        const auto top = static_cast<float>(area.getY());
        g.setColour(kPinColour.withAlpha(0.6f));
        g.drawLine(x, top, x, static_cast<float>(area.getBottom()), 1.0f);
        juce::Path handle;
        handle.addTriangle(x - kPinHandle / 2.0f, top, x + kPinHandle / 2.0f, top, x, top + kPinHandle);
        g.setColour(kPinColour);
        g.fillPath(handle);
    }
}

void TimelineView::paintBeatGuides(juce::Graphics& g)
{
    // The selected record's bar lines, faint, through every other lane (clips or not), so a third or
    // fourth deck can be lined up against it.
    const Set& set = document_.set();
    const Clip* clip = set.findClip(selected_);
    const Track* track = clip != nullptr ? set.findTrack(clip->trackId) : nullptr;
    const auto grid = track != nullptr ? pool_.gridFor(*track) : std::nullopt;
    const auto timing = timings_.find(selected_);
    if (!beatGuides_ || !grid || timing == timings_.end() || numDecks() < 2)
        return;

    forEachVisibleBeat(*clip, timing->second, *track, *grid, xToTime(kHeaderWidth), xToTime(getWidth()),
                       kBeatsPerBar, [&](float x, int64_t beat) {
                           g.setColour(beatColour(beat).withAlpha(0.35f));
                           for (int d = 0; d < numDecks(); ++d)
                               if (d != clip->deck)
                               {
                                   const auto lane = waveformBounds(d);
                                   g.drawLine(x, static_cast<float>(lane.getY()), x,
                                              static_cast<float>(lane.getBottom()), 1.0f);
                               }
                       });
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
    const auto* pass = recorder_ != nullptr ? recorder_->pass(clip.deck, param) : nullptr;
    if (pass != nullptr && pass->clip == clip.id && isTrim(pass->mode) && !pass->moves.empty())
    {
        // Trimming: the curve after the amount, as it's heard, a couple of pixels at a time; Trim
        // from where it began to the playhead (the rest as it is), Trim All the whole clip.
        const bool all = pass->mode == AutomationMode::TrimAll;
        const double from = all ? timing.startSeconds() : pass->moves.front().seconds;
        const double to = all ? timing.endSeconds()
                              : std::min(std::max(playhead_, pass->moves.back().seconds), timing.endSeconds());
        if (!all)
        {
            juce::Graphics::ScopedSaveState state(g);
            const auto x0 = static_cast<int>(std::floor(timeToX(from))), x1 = static_cast<int>(std::ceil(timeToX(to)));
            g.excludeClipRegion({x0, area.getY(), std::max(0, x1 - x0), area.getHeight()});
            paintCurve(g, clip, timing, param, area);
        }
        auto trimmedAt = [&](double seconds) {
            const double amount = all ? pass->moves.back().value : edit::amountAt(pass->moves, seconds);
            return MixerChannel::trimmed(param, valueAt(clip, param, timing.sourceFrameAt(seconds)), amount);
        };
        const double step = 2.0 / pixelsPerSecond_;
        const double first = std::max(from, xToTime(kHeaderWidth) - step);
        const double last = std::min(to, xToTime(getWidth()) + step);
        if (last <= first)
            return; // off screen
        juce::Path line;
        auto point = [&](double seconds) {
            return juce::Point<float>(static_cast<float>(timeToX(seconds)),
                                      static_cast<float>(valueToY(param, trimmedAt(seconds), area)));
        };
        line.startNewSubPath(point(first));
        for (double t = first + step; t < last; t += step)
            line.lineTo(point(t));
        line.lineTo(point(last));
        g.setColour(juce::Colours::black.withAlpha(0.6f));
        g.strokePath(line, juce::PathStrokeType(3.5f));
        g.setColour(paramColour(param));
        g.strokePath(line, juce::PathStrokeType(1.75f));
        return;
    }
    if (pass != nullptr && pass->clip == clip.id && pass->mode == AutomationMode::Latch && !pass->moves.empty())
    {
        // Recording: the old curve is hidden where the pass covers it. (Override passes aren't
        // drawn: they won't change it.)
        const double from = pass->moves.front().seconds;
        const double to = std::min(std::max(playhead_, pass->moves.back().seconds), timing.endSeconds());
        const auto x0 = static_cast<int>(std::floor(timeToX(from))), x1 = static_cast<int>(std::ceil(timeToX(to)));
        {
            juce::Graphics::ScopedSaveState state(g);
            g.excludeClipRegion({x0, area.getY(), std::max(0, x1 - x0), area.getHeight()});
            paintCurve(g, clip, timing, param, area);
        }
        juce::Path line;
        auto point = [&](double seconds, double value) {
            return juce::Point<float>(static_cast<float>(timeToX(seconds)), static_cast<float>(valueToY(param, value, area)));
        };
        line.startNewSubPath(point(from, pass->moves.front().value));
        for (const auto& move : pass->moves)
            line.lineTo(point(move.seconds, move.value));
        line.lineTo(point(to, pass->moves.back().value));
        g.setColour(juce::Colours::black.withAlpha(0.6f));
        g.strokePath(line, juce::PathStrokeType(3.5f));
        g.setColour(paramColour(param));
        g.strokePath(line, juce::PathStrokeType(1.75f));
        return;
    }
    paintCurve(g, clip, timing, param, area);
}

void TimelineView::paintCurve(juce::Graphics& g, const Clip& clip, const ClipTiming& timing, Param param,
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
    else if (auto ends = flatLineEnds(clip, hit.param, hit.frame))
    {
        // On a flat stretch: dragging moves it up and down whole; a click adds a keypoint (mouseUp).
        flatLine_ = {hit, std::move(*ends), hit.value};
        drag_ = Drag::FlatLine;
        return;
    }
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

std::optional<std::vector<int>> TimelineView::flatLineEnds(const Clip& clip, Param param, int64_t frame) const
{
    // The points either side of `frame`, among those that count (from the needle drop on).
    const auto& points = clip.points(param);
    int before = kStartPoint;
    std::optional<int> after;
    for (size_t i = 0; i < points.size(); ++i)
    {
        if (points[i].sourceFrame < clip.inFrame)
            continue;
        if (points[i].sourceFrame <= frame)
            before = static_cast<int>(i);
        else
        {
            after = static_cast<int>(i);
            break;
        }
    }
    const double beforeValue = before == kStartPoint ? baseValue(clip, param) : points[static_cast<size_t>(before)].value;
    if (after && std::abs(points[static_cast<size_t>(*after)].value - beforeValue) > 1e-9)
        return std::nullopt; // a slope
    // Past the last keypoint the line holds its value to the end of the clip: only one end moves.
    std::vector<int> ends{before};
    if (after)
        ends.push_back(*after);
    return ends;
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

// --- Nudges -----------------------------------------------------------------

juce::Rectangle<int> TimelineView::actionArea(const Clip& clip, const ClipTiming& timing, size_t index,
                                             juce::Rectangle<int> waveArea) const
{
    const auto& nudge = clip.actions[index];
    const double from = timing.timelineSecondsAt(static_cast<double>(nudge.sourceFrame));
    const auto x0 = static_cast<int>(std::floor(timeToX(from)));
    const auto x1 = static_cast<int>(std::ceil(timeToX(from + actionLengthSeconds(nudge))));
    // At least wide enough to see and grab when zoomed out.
    return {x0 - 2, waveArea.getBottom() - kNudgeStrip, std::max(x1 - x0, 1) + 4, kNudgeStrip};
}

void TimelineView::paintActions(juce::Graphics& g, const Clip& clip, const ClipTiming& timing, juce::Rectangle<int> waveArea)
{
    if (clip.actions.empty())
        return;
    g.setFont(rulerFont_);
    for (size_t i = 0; i < clip.actions.size(); ++i)
    {
        const auto& action = clip.actions[i];
        if (action.sourceFrame < clip.inFrame || action.sourceFrame >= clip.outFrame)
            continue;
        const auto box = actionArea(clip, timing, i, waveArea);
        if (box.getRight() < kHeaderWidth || box.getX() > getWidth())
            continue;
        const double from = timing.timelineSecondsAt(static_cast<double>(action.sourceFrame));
        const double offset = actionOffsetSeconds(action);
        const auto colour = offset < 0.0 ? kPushColour : kDragColour;

        // A faint line up through the waveform where it starts.
        g.setColour(colour.withAlpha(0.35f));
        g.fillRect(static_cast<float>(box.getX() + 2), static_cast<float>(waveArea.getY()), 1.0f,
                   static_cast<float>(waveArea.getHeight() - kNudgeStrip));

        // The speed curve about a line at normal speed: pushes above it, held back below (down to
        // a standstill at the bottom), each in its colour.
        const auto strip = box.toFloat().reduced(0.0f, 1.0f);
        const float base = strip.getCentreY(), half = strip.getHeight() / 2.0f;
        juce::Path curve;
        curve.startNewSubPath(static_cast<float>(timeToX(from)), base);
        for (const auto& point : speedShape(action))
            curve.lineTo(static_cast<float>(timeToX(from + point.seconds)),
                         base - half * static_cast<float>(std::clamp(point.factor - 1.0, -1.0, 1.0)));
        curve.closeSubPath();
        g.setColour(kDragColour.withAlpha(0.15f));
        g.fillRect(strip.withTrimmedTop(half));
        {
            juce::Graphics::ScopedSaveState state(g);
            g.reduceClipRegion(strip.withHeight(half).toNearestInt());
            g.setColour(kPushColour);
            g.fillPath(curve);
        }
        {
            juce::Graphics::ScopedSaveState state(g);
            g.reduceClipRegion(strip.withTrimmedTop(half).toNearestInt());
            g.setColour(kDragColour);
            g.fillPath(curve);
        }
        // Sized after it, when there's room to read it; drag the number up or down to change it.
        if (const auto label = actionLabelArea(box))
        {
            const bool dragged = drag_ == Drag::ActionAmount && dragOrigin_.id == clip.id && dragAction_ == i;
            const auto text = std::abs(offset) < 1.0 ? juce::String::formatted("%+.1f ms", offset * 1000.0 + 0.0)
                                                     : juce::String::formatted("%+.3f s", offset);
            if (dragged)
            {
                g.setColour(colour.withAlpha(0.3f));
                g.fillRoundedRectangle(label->toFloat(), 3.0f);
            }
            g.setColour(dragged ? juce::Colours::white : colour);
            g.drawText(text, label->reduced(3, 0), juce::Justification::centredLeft, false);
        }
    }
}

std::optional<juce::Rectangle<int>> TimelineView::actionLabelArea(juce::Rectangle<int> box) const
{
    if (pixelsPerSecond_ < 100.0 && box.getWidth() <= 70)
        return std::nullopt;
    return box.withX(box.getRight() + 1).withWidth(70).translated(0, -kNudgeStrip);
}

std::optional<std::pair<Id, size_t>> TimelineView::actionLabelAt(juce::Point<float> position) const
{
    const int deck = deckAt(static_cast<int>(position.y));
    if (deck < 0)
        return std::nullopt;
    const auto waveArea = waveformBounds(deck).withTrimmedTop(18);
    for (const auto& clip : document_.set().clips)
        if (auto it = timings_.find(clip.id); clip.deck == deck && it != timings_.end())
            for (size_t i = 0; i < clip.actions.size(); ++i)
                if (const auto label = actionLabelArea(actionArea(clip, it->second, i, waveArea));
                    !clip.actions[i].linked && clip.actions[i].sourceFrame >= clip.inFrame && label && label->toFloat().contains(position))
                    return std::make_pair(clip.id, i);
    return std::nullopt;
}

std::optional<std::pair<Id, size_t>> TimelineView::actionAt(juce::Point<float> position) const
{
    const int deck = deckAt(static_cast<int>(position.y));
    if (deck < 0)
        return std::nullopt;
    const auto waveArea = waveformBounds(deck).withTrimmedTop(18);
    for (const auto& clip : document_.set().clips)
        if (auto it = timings_.find(clip.id); clip.deck == deck && it != timings_.end())
            for (size_t i = 0; i < clip.actions.size(); ++i)
                if (!clip.actions[i].linked && clip.actions[i].sourceFrame >= clip.inFrame &&
                    actionArea(clip, it->second, i, waveArea).expanded(3, 1).toFloat().contains(position))
                    return std::make_pair(clip.id, i);
    return std::nullopt;
}

void TimelineView::showActionMenu(Id clip, size_t index)
{
    const Clip* c = document_.set().findClip(clip);
    if (c == nullptr || index >= c->actions.size())
        return;
    const auto action = c->actions[index];
    const bool nudge = action.kind == DeckAction::Kind::Nudge;
    // Peak speeds a nudge can be set to, as a fraction of the record's speed (10% is how it was played).
    static constexpr double kSpeeds[] = {0.02, 0.05, 0.1, 0.2, 0.4};
    constexpr int kSpeedBase = 100;
    juce::PopupMenu menu;
    menu.addItem(1, nudge ? "Delete nudge" : "Delete touch");
    menu.addItem(2, "Clear all nudges and touches on this record");
    if (nudge)
    {
        juce::PopupMenu speeds;
        for (size_t k = 0; k < std::size(kSpeeds); ++k)
            speeds.addItem(kSpeedBase + static_cast<int>(k),
                           juce::String::formatted("%.0f%% %s, over %.0f ms", kSpeeds[k] * 100.0,
                                                   action.seconds < 0.0 ? "faster" : "slower",
                                                   nudgeLengthSeconds(action.seconds, kSpeeds[k]) * 1000.0),
                           true, std::abs(action.speed - kSpeeds[k]) < 1e-9);
        menu.addSubMenu("Speed", speeds);
    }
    menu.showMenuAsync(juce::PopupMenu::Options(), [safe = juce::Component::SafePointer(this), clip, index](int result) {
        if (safe == nullptr || result == 0)
            return;
        safe->document_.edit([&](Set& s) {
            return edit::withFollowers(s, clip, [&](Set& t) {
                if (result >= kSpeedBase)
                    return edit::setNudgeSpeed(t, clip, index, kSpeeds[static_cast<size_t>(result - kSpeedBase)]);
                return result == 1 ? edit::deleteAction(t, clip, index) : edit::clearActions(t, clip);
            });
        });
    });
}

// --- Right-click on a record --------------------------------------------------

void TimelineView::showClipMenu(Id clipId, double atSeconds)
{
    const Clip* clip = document_.set().findClip(clipId);
    if (clip == nullptr)
        return;
    struct Insert
    {
        const char* text;
        bool hold;
        double seconds;
    };
    static constexpr Insert kInserts[] = {
        {"Push 10 ms earlier", false, -0.010}, {"Push 1 ms earlier", false, -0.001},
        {"Hold back 10 ms", false, 0.010},     {"Hold back 1 ms", false, 0.001},
        {"Hold still 0.25 s", true, 0.25},     {"Hold still 0.5 s", true, 0.5},
        {"Hold still 1 s", true, 1.0},
    };
    constexpr int kCleanUp = 100;
    constexpr int kLineUp = 101;
    constexpr int kUnlink = 102;
    juce::PopupMenu menu;
    menu.addSectionHeader("Here, at " + formatTime(atSeconds, true));
    for (size_t i = 0; i < std::size(kInserts); ++i)
    {
        if (i == 4)
            menu.addSeparator();
        menu.addItem(static_cast<int>(i) + 1, kInserts[i].text);
    }
    menu.addSeparator();
    menu.addItem(kLineUp, "Line up beats here", onLineUpBeats != nullptr);
    const bool linked = std::any_of(document_.set().links.begin(), document_.set().links.end(),
                                    [clipId](const BeatLink& l) { return l.leader == clipId || l.follower == clipId; });
    menu.addItem(kUnlink, "Delete beat links", linked);
    menu.addItem(kCleanUp, "Clean up nudges...", !clip->actions.empty());
    menu.showMenuAsync(juce::PopupMenu::Options(), [safe = juce::Component::SafePointer(this), clipId, atSeconds](int result) {
        if (safe == nullptr || result == 0)
            return;
        if (result == kUnlink)
        {
            safe->document_.edit([&](Set& s) { return edit::deleteLinks(s, clipId); });
            return;
        }
        if (result == kLineUp)
        {
            safe->onLineUpBeats(clipId, atSeconds);
            return;
        }
        if (result == kCleanUp)
        {
            safe->cleanUp_ = CleanUp{clipId, false, {}, {}};
            safe->setMouseCursor(juce::MouseCursor::CrosshairCursor);
            safe->repaint();
            return;
        }
        const auto& insert = kInserts[static_cast<size_t>(result - 1)];
        safe->document_.edit([&](Set& s) {
            return edit::withFollowers(s, clipId, [&](Set& t) {
                return insert.hold ? edit::addHold(t, clipId, atSeconds, insert.seconds)
                                   : edit::addNudge(t, clipId, atSeconds, insert.seconds);
            });
        });
    });
}

void TimelineView::paintCleanUp(juce::Graphics& g)
{
    const Clip* clip = document_.set().findClip(cleanUp_->clip);
    if (clip == nullptr)
        return;
    const auto lane = laneBounds(clip->deck);
    if (!cleanUp_->dragging)
    {
        g.setColour(juce::Colours::black.withAlpha(0.7f));
        const auto hint = lane.withTrimmedLeft(kHeaderWidth).withHeight(22).translated(0, 20);
        g.fillRect(hint.withWidth(440));
        g.setColour(juce::Colours::white);
        g.setFont(titleFont_);
        g.drawText("Drag over the stretch of this record to clean up its nudges (Esc cancels)", hint.reduced(8, 0),
                   juce::Justification::centredLeft, false);
        return;
    }
    const auto box = juce::Rectangle<float>(cleanUp_->from, cleanUp_->to);
    g.setColour(juce::Colours::white.withAlpha(0.12f));
    g.fillRect(box);
    g.setColour(juce::Colours::white);
    g.drawRect(box, 1.0f);
}

bool TimelineView::keyPressed(const juce::KeyPress& key)
{
    if (cleanUp_ && key == juce::KeyPress::escapeKey)
    {
        cleanUp_.reset();
        setMouseCursor(juce::MouseCursor::NormalCursor);
        repaint();
        return true;
    }
    return false;
}

// --- A hand on the record ----------------------------------------------------

std::optional<size_t> TimelineView::touchIndex() const
{
    const Clip* clip = document_.set().findClip(touch_.clip);
    if (clip == nullptr)
        return std::nullopt;
    for (size_t i = 0; i < clip->actions.size(); ++i)
        if (clip->actions[i].kind == DeckAction::Kind::Touch && clip->actions[i].sourceFrame == touch_.frame)
            return i;
    return std::nullopt;
}

double TimelineView::heardNow() const
{
    return heardSeconds ? heardSeconds() : engine_.playheadSeconds();
}

bool TimelineView::startTouch()
{
    const auto timing = timings_.find(touch_.clip);
    const double heard = heardNow();
    if (!engine_.isPlaying() || timing == timings_.end() || heard < timing->second.startSeconds() ||
        heard >= timing->second.endSeconds())
        return false;
    touch_.active = true;
    touch_.start = heard;
    touch_.frame = static_cast<int64_t>(std::llround(timing->second.sourceFrameAt(heard)));
    touch_.handX = touch_.mouseX;
    touch_.music = 0.0;
    touch_.speeds = {{0.0, 1.0}};
    sampleTouch();
    return true;
}

void TimelineView::sampleTouch()
{
    // Where the hand has taken the record: dragging the waveform left by a second of the timeline
    // takes it a second of music on; holding the mouse still holds the record still. Never back.
    const double t = heardNow() - touch_.start;
    const double music = std::max(touch_.music, (touch_.handX - touch_.mouseX) / pixelsPerSecond_);
    const auto& last = touch_.speeds.back();
    constexpr double kMinStep = 0.01; // UI ticks come about every 16 ms; closer ones are noise
    if (t - last.seconds >= kMinStep)
    {
        touch_.speeds.push_back({t, (music - touch_.music) / (t - last.seconds)});
        touch_.music = music;
    }
    // Written as far as it's gone, holding its last speed a little ahead of what's heard, so the
    // engine (which plays ahead of what's heard) never plays past it.
    auto speeds = touch_.speeds;
    speeds.push_back({std::max(t, speeds.back().seconds) + kTouchLeadSeconds, speeds.back().factor});
    const auto touch = touch_;
    document_.edit([&](Set& s) {
        if (const auto index = touchIndex())
            edit::deleteAction(s, touch.clip, *index);
        return edit::addTouch(s, touch.clip, touch.start, speeds);
    });
}

void TimelineView::endTouch()
{
    if (!touch_.active)
    {
        touch_ = {};
        return;
    }
    const auto touch = touch_;
    if (const auto index = touchIndex())
        document_.edit([&](Set& s) { return edit::deleteAction(s, touch.clip, *index); });
    // Written for good as played, and this time the records mixed in later move with it.
    document_.edit([&](Set& s) {
        return edit::withFollowers(s, touch.clip, [&](Set& t) { return edit::addTouch(t, touch.clip, touch.start, touch.speeds); });
    });
    touch_ = {};
}

// --- Mouse ------------------------------------------------------------------

void TimelineView::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();

    if (cleanUp_)
    {
        if (e.mods.isPopupMenu())
        {
            cleanUp_.reset();
            setMouseCursor(juce::MouseCursor::NormalCursor);
        }
        else
        {
            cleanUp_->dragging = true;
            cleanUp_->from = cleanUp_->to = e.position;
        }
        repaint();
        return;
    }

    if (e.y < kRulerHeight && e.x >= kHeaderWidth)
    {
        drag_ = Drag::Scrub;
        engine_.seek(std::max(0.0, xToTime(e.x)));
        return;
    }

    if (const auto end = linkEndAt(e.position))
    {
        setSelectedClip(end->clip);
        if (e.mods.isPopupMenu())
        {
            showLinkMenu(end->link, end->clip);
            return;
        }
        dragLink_ = end->link;
        dragOrigin_ = *document_.set().findClip(end->clip);
        dragTiming_ = timings_.at(end->clip);
        drag_ = Drag::LinkEnd;
        document_.beginGesture();
        return;
    }

    {
        auto beat = beatPinAt(e.position);
        if (beat && e.mods.isPopupMenu())
        {
            setSelectedClip(beat->clip);
            showBeatPinMenu(beat->clip, beat->beat);
            return;
        }
        if (!beat && e.mods.isCommandDown() && !e.mods.isPopupMenu())
            beat = beatLineAt(e.position);
        const Hit body = clipAt(e.position);
        const Id clipId = beat ? beat->clip : body.part == Part::Body && e.mods.isCommandDown() ? body.clip : 0;
        if (clipId != 0 && !e.mods.isPopupMenu())
        {
            // A beat line: pinned and dragged to where the beat really is (pinned in place by a
            // click). Into another lane, from a beat line or anywhere in the record: a link.
            setSelectedClip(clipId);
            const Set& set = document_.set();
            dragOrigin_ = *set.findClip(clipId);
            const Track* track = set.findTrack(dragOrigin_.trackId);
            dragGrid_ = pool_.gridFor(*track).value_or(BeatGrid{});
            dragTrackGrid_ = track->grid;
            dragHasBeat_ = beat.has_value();
            dragBeat_ = beat ? beat->beat : 0;
            dragTiming_ = timings_.at(clipId);
            dragAnchorTime_ = xToTime(e.x);
            dragLinkFrame_ = beat ? std::llround(dragGrid_.secondsOfBeat(static_cast<double>(dragBeat_)) * track->sampleRate)
                                  : std::llround(dragTiming_->sourceFrameAt(dragAnchorTime_));
            linkTo_.reset();
            drag_ = Drag::BeatPin;
            document_.beginGesture();
            return;
        }
    }

    if (const auto label = actionLabelAt(e.position); label && !e.mods.isPopupMenu())
    {
        // The number: dragged up or down, it changes the action's size.
        setSelectedClip(label->first);
        dragOrigin_ = *document_.set().findClip(label->first);
        dragAction_ = label->second;
        dragActionAmount_ = actionOffsetSeconds(dragOrigin_.actions[label->second]);
        drag_ = Drag::ActionAmount;
        document_.beginGesture();
        repaint();
        return;
    }

    if (const auto action = actionAt(e.position))
    {
        setSelectedClip(action->first);
        if (e.mods.isPopupMenu())
            showActionMenu(action->first, action->second);
        else if (!engine_.isPlaying())
        {
            // Stopped, it can be dragged along the music. Positions come from the timing at the
            // start of the drag, as for keypoints: moving it changes the timing.
            dragOrigin_ = *document_.set().findClip(action->first);
            dragTiming_ = timings_.at(action->first);
            dragAction_ = action->second;
            dragAnchorTime_ = xToTime(e.x);
            dragActionStart_ = dragTiming_->timelineSecondsAt(
                static_cast<double>(dragOrigin_.actions[action->second].sourceFrame));
            drag_ = Drag::Action;
            document_.beginGesture();
        }
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
        // Only the ruler moves the playhead: a stray click in a lane shouldn't lose your place.
        setSelectedClip(0);
        return;
    }

    setSelectedClip(hit.clip);
    if (e.mods.isPopupMenu())
    {
        showClipMenu(hit.clip, xToTime(e.x));
        return;
    }
    dragOrigin_ = *document_.set().findClip(hit.clip);
    dragAnchorTime_ = xToTime(e.x);
    drag_ = hit.part == Part::StartEdge ? Drag::TrimStart : hit.part == Part::EndEdge ? Drag::TrimEnd : Drag::Move;
    document_.beginGesture();
    // While the record plays, its body is under the hand (see the class comment): Option moves it
    // instead, with the tone arm, and Shift keeps its own meanings. A record that isn't playing
    // now moves as when stopped.
    const auto timing = timings_.find(hit.clip);
    const bool heard = timing != timings_.end() && heardNow() >= timing->second.startSeconds() &&
                       heardNow() < timing->second.endSeconds();
    if (drag_ == Drag::Move && engine_.isPlaying() && heard && !e.mods.isAltDown() && !e.mods.isShiftDown() &&
        !e.mods.isPopupMenu())
    {
        drag_ = Drag::Touch;
        touch_ = {};
        touch_.clip = hit.clip;
        touch_.mouseX = e.position.x;
        touch_.pressClock = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    }
}

void TimelineView::mouseDrag(const juce::MouseEvent& e)
{
    if (cleanUp_)
    {
        if (cleanUp_->dragging)
            cleanUp_->to = e.position;
        repaint();
        return;
    }
    const double t = xToTime(e.x);
    const Id id = dragOrigin_.id;
    if (drag_ == Drag::Touch)
    {
        touch_.mouseX = e.position.x;
        if (!touch_.active && e.getDistanceFromDragStart() >= kTouchDragPixels)
            startTouch();
        return;
    }
    switch (drag_)
    {
    case Drag::Scrub:
        engine_.seek(std::max(0.0, t));
        break;
    case Drag::Touch:
        break;
    case Drag::ActionAmount:
    {
        // Up for more (later), down for less: 0.1 ms a pixel, or 0.01 ms with Shift.
        const double perPixel = e.mods.isShiftDown() ? 0.00001 : 0.0001;
        const double amount = dragActionAmount_ - e.getDistanceFromDragStartY() * perPixel;
        const auto index = dragAction_;
        document_.edit([&](Set& s) {
            return edit::withFollowers(s, id, [&](Set& u) { return edit::setActionOffset(u, id, index, amount); });
        });
        break;
    }
    case Drag::LinkEnd:
    {
        const auto frame = snappedFrame(dragOrigin_, *dragTiming_, e.position.x);
        const Id link = dragLink_;
        document_.edit([&](Set& s) { return edit::moveLink(s, link, id, frame); });
        break;
    }
    case Drag::BeatPin:
    {
        const Track* track = document_.set().findTrack(dragOrigin_.trackId);
        if (track == nullptr)
            break;
        const Id trackId = track->id;
        const int deck = deckAt(e.y);
        if (deck >= 0 && deck != dragOrigin_.deck)
        {
            // Over another lane: a link, so the grid goes back to how it was.
            linkTo_ = e.position;
            const auto original = dragTrackGrid_;
            document_.edit([&](Set& s) { return edit::setTrackGrid(s, trackId, original); });
            repaint();
            break;
        }
        if (linkTo_)
        {
            linkTo_.reset();
            repaint();
        }
        if (!dragHasBeat_)
            break;
        const double from = dragTiming_->timelineSecondsAt(dragGrid_.secondsOfBeat(static_cast<double>(dragBeat_)) * track->sampleRate);
        const double at = from + (t - dragAnchorTime_) * (e.mods.isShiftDown() ? kFineDragFactor : 1.0);
        const BeatGrid updated = dragGrid_.pinned(dragBeat_, dragTiming_->sourceFrameAt(at) / track->sampleRate);
        document_.edit([&](Set& s) { return edit::setTrackGrid(s, trackId, updated); });
        break;
    }
    case Drag::Action:
    {
        const double at = dragActionStart_ + (t - dragAnchorTime_) * (e.mods.isShiftDown() ? kFineDragFactor : 1.0);
        const auto frame = static_cast<int64_t>(std::llround(dragTiming_->sourceFrameAt(at)));
        const auto index = dragAction_;
        document_.edit([&](Set& s) {
            return edit::withFollowers(s, id, [&](Set& u) { return edit::moveAction(u, id, index, frame); });
        });
        break;
    }
    case Drag::Move:
    {
        // Shift drags at a tenth of the speed for fine alignment.
        const double delta = (t - dragAnchorTime_) * (e.mods.isShiftDown() ? kFineDragFactor : 1.0);
        const int deck = deckAt(e.y);
        // With the tone arm (Option) the record is lifted off and dropped elsewhere: its beat
        // links go. A plain drag keeps them (a follower stays put; a leader takes its followers).
        const bool toneArm = e.mods.isAltDown() && e.getDistanceFromDragStart() > 0;
        document_.edit([&](Set& s) {
            const bool unlinked = toneArm && edit::deleteLinks(s, id);
            return edit::moveClip(s, id, dragOrigin_.startSeconds + delta, deck >= 0 ? deck : dragOrigin_.deck) ||
                   unlinked;
        });
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
    case Drag::FlatLine:
    {
        const auto area = automationBounds(dragOrigin_.deck);
        const double delta = yToValue(dragParam_, e.position.y, area) -
                             yToValue(dragParam_, static_cast<float>(e.mouseDownPosition.y), area);
        const auto param = dragParam_;
        const double value = flatLine_.value + delta;
        const auto ends = flatLine_.ends;
        const auto& points = dragOrigin_.points(param);
        document_.edit([&](Set& s) {
            bool changed = false;
            for (int index : ends)
                changed = (index == kStartPoint
                               ? edit::setStartValue(s, id, param, value)
                               : edit::moveAutomationPoint(s, id, param, static_cast<size_t>(index),
                                                           points[static_cast<size_t>(index)].sourceFrame, value)) ||
                          changed;
            return changed;
        });
        break;
    }
    case Drag::None:
        break;
    }
}

void TimelineView::mouseUp(const juce::MouseEvent& e)
{
    if (cleanUp_)
    {
        if (cleanUp_->dragging)
        {
            const auto clean = *cleanUp_;
            cleanUp_.reset();
            setMouseCursor(juce::MouseCursor::NormalCursor);
            const double from = xToTime(std::min(clean.from.x, clean.to.x));
            const double to = xToTime(std::max(clean.from.x, clean.to.x));
            document_.edit([&](Set& s) {
                return edit::withFollowers(s, clean.clip,
                                           [&](Set& t) { return edit::tidyActions(t, clean.clip, from, to).has_value(); });
            });
            repaint();
        }
        return;
    }
    // A click on a flat line without dragging adds a keypoint there, as on any other line.
    if (drag_ == Drag::FlatLine && !e.mouseWasDraggedSinceMouseDown())
    {
        const auto hit = flatLine_.hit;
        document_.edit([&](Set& s) { return edit::setAutomationPoint(s, hit.clip, hit.param, hit.frame, hit.value); });
    }
    if (drag_ == Drag::Touch)
        endTouch();
    if (drag_ == Drag::BeatPin)
    {
        if (linkTo_)
        {
            const Hit target = clipAt(*linkTo_);
            const Clip* other = document_.set().findClip(target.clip);
            if (other != nullptr && other->deck != dragOrigin_.deck)
            {
                const auto frame = snappedFrame(*other, timings_.at(other->id), linkTo_->x);
                const Id from = dragOrigin_.id, to = other->id;
                const auto fromFrame = dragLinkFrame_;
                document_.edit([&](Set& s) { return edit::addLink(s, from, fromFrame, to, frame).has_value(); });
            }
            linkTo_.reset();
            repaint();
        }
        else if (!e.mouseWasDraggedSinceMouseDown() && dragTiming_)
        {
            // A click: the downbeat is about here; Shift, another beat is (the tempo is found
            // from the music between and around them).
            const Track* track = document_.set().findTrack(dragOrigin_.trackId);
            const double seconds = dragTiming_->sourceFrameAt(xToTime(e.mouseDownPosition.x)) / track->sampleRate;
            const std::optional<double> downbeat =
                downbeatClick_ && downbeatClick_->first == track->id ? std::optional(downbeatClick_->second) : std::nullopt;
            if (!e.mods.isShiftDown())
            {
                downbeatClick_ = {track->id, seconds};
                if (onDownbeatClicked)
                    onDownbeatClicked(track->id, seconds);
            }
            else if (onBeatClicked)
                onBeatClicked(track->id, downbeat, seconds);
        }
    }
    if (drag_ == Drag::Move || drag_ == Drag::TrimStart || drag_ == Drag::TrimEnd || drag_ == Drag::Keypoint ||
        drag_ == Drag::FlatLine || drag_ == Drag::Touch || drag_ == Drag::Action || drag_ == Drag::ActionAmount ||
        drag_ == Drag::BeatPin || drag_ == Drag::LinkEnd)
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
        const double nearest = grid->secondsOfBeat(std::round(grid->beatAt(beat)));
        const double nearestX = timeToX(timing->second.timelineSecondsAt(nearest * track->sampleRate));
        if (std::abs(nearestX - x) <= kSnapPixels)
            beat = nearest;
    }
    const BeatGrid updated = grid->withDownbeatAt(beat);
    const Id trackId = track->id;
    document_.edit([&](Set& s) { return edit::setTrackGrid(s, trackId, updated); });
}

std::optional<TimelineView::BeatHit> TimelineView::beatLineAt(juce::Point<float> position) const
{
    const Hit hit = clipAt(position);
    const Set& set = document_.set();
    const Clip* clip = set.findClip(hit.clip);
    const Track* track = clip != nullptr ? set.findTrack(clip->trackId) : nullptr;
    const auto grid = track != nullptr ? pool_.gridFor(*track) : std::nullopt;
    if (hit.part != Part::Body || !grid || position.y < static_cast<float>(waveformBounds(clip->deck).getY() + 18))
        return std::nullopt;
    const auto& timing = timings_.at(clip->id);
    const double t = xToTime(position.x);
    const double source = timing.sourceFrameAt(t) / track->sampleRate;
    // Only where every beat is drawn: a line you can't see can't be grabbed.
    if (grid->periodAt(source) / timing.speedAt(t) * pixelsPerSecond_ < kMinBeatSpacingPixels)
        return std::nullopt;
    const auto beat = static_cast<int64_t>(std::llround(grid->beatAt(source)));
    const double x = timeToX(timing.timelineSecondsAt(grid->secondsOfBeat(static_cast<double>(beat)) * track->sampleRate));
    if (std::abs(x - position.x) > kBeatGrabPixels)
        return std::nullopt;
    return BeatHit{clip->id, beat};
}

std::optional<TimelineView::BeatHit> TimelineView::beatPinAt(juce::Point<float> position) const
{
    const Hit hit = clipAt(position);
    const Set& set = document_.set();
    const Clip* clip = set.findClip(hit.clip);
    const Track* track = clip != nullptr ? set.findTrack(clip->trackId) : nullptr;
    const auto grid = track != nullptr ? pool_.gridFor(*track) : std::nullopt;
    if (!grid || grid->pins.empty())
        return std::nullopt;
    const int top = waveformBounds(clip->deck).getY() + 18;
    if (position.y < static_cast<float>(top) || position.y > static_cast<float>(top + kPinHandle + 2))
        return std::nullopt;
    const auto& timing = timings_.at(clip->id);
    std::optional<BeatHit> best;
    double bestDistance = kPinHandle / 2.0 + 1.0;
    for (const auto& pin : grid->pins)
    {
        const double distance = std::abs(timeToX(timing.timelineSecondsAt(pin.seconds * track->sampleRate)) - position.x);
        if (distance <= bestDistance)
        {
            bestDistance = distance;
            best = BeatHit{clip->id, pin.beat};
        }
    }
    return best;
}

int64_t TimelineView::snappedFrame(const Clip& clip, const ClipTiming& timing, float x) const
{
    const double frame = timing.sourceFrameAt(xToTime(x));
    const Track* track = document_.set().findTrack(clip.trackId);
    if (const auto grid = track != nullptr ? pool_.gridFor(*track) : std::nullopt)
    {
        const double beat = grid->secondsOfBeat(std::round(grid->beatAt(frame / track->sampleRate))) * track->sampleRate;
        if (std::abs(timeToX(timing.timelineSecondsAt(beat)) - x) <= kBeatGrabPixels)
            return std::llround(beat);
    }
    return std::llround(frame);
}

std::optional<TimelineView::LinkHit> TimelineView::linkEndAt(juce::Point<float> position) const
{
    constexpr float kGrab = 6.0f;
    for (const auto& link : document_.set().links)
        for (const auto& [clipId, frame] : {std::pair{link.follower, link.followerFrame}, std::pair{link.leader, link.leaderFrame}})
        {
            const Clip* clip = document_.set().findClip(clipId);
            const auto timing = timings_.find(clipId);
            if (clip == nullptr || timing == timings_.end())
                continue;
            const auto lane = waveformBounds(clip->deck);
            const juce::Point<float> dot(static_cast<float>(timeToX(timing->second.timelineSecondsAt(static_cast<double>(frame)))),
                                         static_cast<float>(lane.getCentreY()));
            if (dot.getDistanceFrom(position) <= kGrab)
                return LinkHit{link.id, clipId};
        }
    return std::nullopt;
}

void TimelineView::paintLinks(juce::Graphics& g)
{
    const Set& set = document_.set();
    struct End
    {
        double seconds;
        juce::Point<float> dot;
    };
    const auto end = [&](Id clipId, int64_t frame) -> std::optional<End> {
        const Clip* clip = set.findClip(clipId);
        const auto timing = timings_.find(clipId);
        if (clip == nullptr || timing == timings_.end() || frame < clip->inFrame || frame >= clip->outFrame)
            return std::nullopt;
        const double seconds = timing->second.timelineSecondsAt(static_cast<double>(frame));
        return End{seconds, {static_cast<float>(timeToX(seconds)), static_cast<float>(waveformBounds(clip->deck).getCentreY())}};
    };
    for (const auto& link : set.links)
    {
        const auto a = end(link.leader, link.leaderFrame);
        const auto b = end(link.follower, link.followerFrame);
        if (!a || !b)
            continue;
        // Red when it can't be kept (validate() says why).
        g.setColour(std::abs(a->seconds - b->seconds) <= 0.001 ? kLinkColour : juce::Colours::red);
        g.drawLine({a->dot, b->dot}, 2.0f);
        for (const auto& p : {a->dot, b->dot})
            g.fillEllipse(juce::Rectangle<float>(9.0f, 9.0f).withCentre(p));
    }
    // A link being made.
    if (drag_ == Drag::BeatPin && linkTo_ && dragTiming_)
    {
        const juce::Point<float> from(
            static_cast<float>(timeToX(dragTiming_->timelineSecondsAt(static_cast<double>(dragLinkFrame_)))),
            static_cast<float>(waveformBounds(dragOrigin_.deck).getCentreY()));
        g.setColour(kLinkColour.withAlpha(0.7f));
        g.drawLine({from, *linkTo_}, 1.5f);
        g.fillEllipse(juce::Rectangle<float>(9.0f, 9.0f).withCentre(from));
    }
}

void TimelineView::showLinkMenu(Id link, Id clipId)
{
    juce::PopupMenu menu;
    menu.addItem(1, "Delete this link");
    menu.addItem(2, "Delete all of this record's links");
    menu.showMenuAsync(juce::PopupMenu::Options(), [safe = juce::Component::SafePointer(this), link, clipId](int result) {
        if (safe == nullptr || result == 0)
            return;
        safe->document_.edit([&](Set& s) { return result == 1 ? edit::deleteLink(s, link) : edit::deleteLinks(s, clipId); });
    });
}

void TimelineView::showBeatPinMenu(Id clipId, int64_t beat)
{
    juce::PopupMenu menu;
    menu.addItem(1, "Unpin this beat");
    menu.addItem(2, "Unpin all beats");
    menu.showMenuAsync(juce::PopupMenu::Options(), [safe = juce::Component::SafePointer(this), clipId, beat](int result) {
        if (safe == nullptr || result == 0)
            return;
        const Set& set = safe->document_.set();
        const Clip* clip = set.findClip(clipId);
        const Track* track = clip != nullptr ? set.findTrack(clip->trackId) : nullptr;
        const auto grid = track != nullptr ? safe->pool_.gridFor(*track) : std::nullopt;
        if (!grid)
            return;
        const BeatGrid updated = result == 1 ? grid->unpinned(beat) : BeatGrid{grid->bpm, grid->firstBeatSeconds};
        const Id trackId = track->id;
        safe->document_.edit([&](Set& s) { return edit::setTrackGrid(s, trackId, updated); });
    });
}

void TimelineView::mouseMove(const juce::MouseEvent& e)
{
    if (cleanUp_)
    {
        setMouseCursor(juce::MouseCursor::CrosshairCursor);
        return;
    }
    if (actionLabelAt(e.position))
    {
        setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
        return;
    }
    if (linkEndAt(e.position))
    {
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        return;
    }
    if (beatPinAt(e.position) || (e.mods.isCommandDown() && beatLineAt(e.position)))
    {
        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
        return;
    }
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
    if (const auto end = linkEndAt(e.position))
    {
        const Id link = end->link;
        document_.edit([&](Set& s) { return edit::deleteLink(s, link); });
        return;
    }
    if (const auto pin = beatPinAt(e.position))
    {
        const Set& set = document_.set();
        const Track* track = set.findTrack(set.findClip(pin->clip)->trackId);
        if (const auto grid = pool_.gridFor(*track))
        {
            const BeatGrid updated = grid->unpinned(pin->beat);
            const Id trackId = track->id;
            document_.edit([&](Set& s) { return edit::setTrackGrid(s, trackId, updated); });
        }
        return;
    }
    if (const auto nudge = actionAt(e.position))
    {
        document_.edit([&](Set& s) {
            return edit::withFollowers(s, nudge->first, [&](Set& t) { return edit::deleteAction(t, nudge->first, nudge->second); });
        });
        return;
    }
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
    return dynamic_cast<LibraryTree*>(details.sourceComponent.get()) != nullptr;
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
    auto* tree = dynamic_cast<LibraryTree*>(details.sourceComponent.get());
    if (tree == nullptr)
        return;
    juce::Array<juce::File> files;
    for (const auto& file : tree->selectedFiles())
        if (pool_.canLoad(file))
            files.add(file);
    dropFiles(files, details.localPosition.x, details.localPosition.y);
}

} // namespace vmp

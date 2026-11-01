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

    if (follow_ && engine_.isPlaying() && drag_ == Drag::None)
    {
        const double span = laneWidth() / pixelsPerSecond_;
        if (playhead_ > viewStart_ + span * 0.95 || playhead_ < viewStart_)
            viewStart_ = playhead_ - span * 0.1;
    }
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

void TimelineView::zoomIn()
{
    const double span = laneWidth() / pixelsPerSecond_;
    const bool playheadVisible = playhead_ >= viewStart_ && playhead_ <= viewStart_ + span;
    zoomAround(2.0, playheadVisible ? playhead_ : viewStart_ + span / 2.0);
}

void TimelineView::zoomOut()
{
    const double span = laneWidth() / pixelsPerSecond_;
    zoomAround(0.5, viewStart_ + span / 2.0);
}

void TimelineView::zoomToSet()
{
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
    auto it = timings_.find(selected_);
    if (it == timings_.end())
        zoomToSet();
    else
        zoomToRange(it->second.startSeconds(), it->second.endSeconds());
}

void TimelineView::zoomToTransition()
{
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
        g.drawText("Deck " + juce::String(d + 1), lane.withWidth(kHeaderWidth).withTrimmedLeft(10),
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
    const auto lane = laneBounds(clip.deck).reduced(0, 4);
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

    // Pitch moves, where they start.
    g.setColour(juce::Colours::white.withAlpha(0.6f));
    for (const auto& move : clip.pitchMoves)
    {
        const auto mx = static_cast<float>(timeToX(timing.timelineSecondsAt(static_cast<double>(move.sourceFrame))));
        g.drawLine(mx, static_cast<float>(area.getY()), mx, static_cast<float>(area.getBottom()), 1.0f);
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
    // Tempo and key as heard, i.e. after the pitch fader.
    const double speed = speedFromPitchSteps(clip.pitchSteps);
    juce::String text = title + "   " + juce::String::formatted("%+.2f%%", pitchPercentFromSteps(clip.pitchSteps) + 0.0);
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
        const bool downbeat = ((k % 4) + 4) % 4 == 0;
        g.setColour(downbeat ? juce::Colours::red.withAlpha(0.9f) : juce::Colours::white.withAlpha(0.45f));
        g.drawLine(x, static_cast<float>(area.getY()), x, static_cast<float>(area.getBottom()), downbeat ? 2.0f : 1.0f);
    }
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
    case Drag::None:
        break;
    }
}

void TimelineView::mouseUp(const juce::MouseEvent&)
{
    if (drag_ == Drag::Move || drag_ == Drag::TrimStart || drag_ == Drag::TrimEnd)
        document_.endGesture();
    drag_ = Drag::None;
}

void TimelineView::mouseMove(const juce::MouseEvent& e)
{
    const Hit hit = clipAt(e.position);
    setMouseCursor(hit.part == Part::StartEdge || hit.part == Part::EndEdge ? juce::MouseCursor::LeftRightResizeCursor
                                                                            : juce::MouseCursor::NormalCursor);
}

void TimelineView::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (clipAt(e.position).clip != 0)
        zoomToClip();
}

void TimelineView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        zoomAround(std::pow(2.0, wheel.deltaY * 4.0), xToTime(e.x));
        return;
    }
    // Lanes fit the window, so both scroll directions pan through time.
    const double pixels = (wheel.deltaX + wheel.deltaY) * 300.0;
    setView(viewStart_ - pixels / pixelsPerSecond_, pixelsPerSecond_);
}

void TimelineView::mouseMagnify(const juce::MouseEvent& e, float scaleFactor)
{
    zoomAround(scaleFactor, xToTime(e.x));
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

#pragma once

#include "app/AudioPool.h"
#include "app/Display.h"
#include "app/Faders.h"
#include "core/Document.h"
#include "core/MixRecorder.h"
#include "engine/Engine.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <map>
#include <optional>

namespace vmp
{

// One lane per deck, clips drawn as 3-band waveforms on a shared time axis (seconds, no tempo
// grid). Each clip shows its own beat lines once zoomed in far enough to judge phase.
// Drag a clip to move it in time or to another deck, drag its edges to trim. Click the ruler
// to move the playhead. Cmd+scroll or pinch to zoom, scroll to pan.
//
// Each lane can show any of its controls' automation over the waveforms (check boxes in the lane
// header). Click on a line to add a keypoint there and drag it; drag keypoints to move them,
// double-click to delete, right-click a line or keypoint for more. The square at the clip start
// is the starting value. Clicking anywhere else moves the clip as usual.
//
// Shift-click a clip to put its downbeat there: on the beat line under the click if there is one
// (fixing which beat is "1" without moving the grid), otherwise exactly where clicked; add Option
// to always use the exact spot. Shift-drag moves the clip finely.
//
// While playing, the view keeps the playhead centred and scrolls the tracks past it. Scrolling
// away pauses that until the playhead reaches the centre of the view, or until showPlayhead().
// Zooming while following zooms around the playhead.
class TimelineView : public juce::Component, public juce::FileDragAndDropTarget, public juce::DragAndDropTarget
{
public:
    TimelineView(SetDocument& document, AudioPool& pool, Engine& engine, const DisplayOptions& display);

    // The set or loaded audio changed.
    void refresh();
    // Called from the UI timer.
    void updatePlayhead();
    // Points each deck's pitch fader at its clip (clipForDeck). Called from the UI timer.
    void updatePitchFaders(bool playing);

    // How tall the lanes are, all together: a quarter, half, three quarters or all of the height
    // they could have.
    enum class LaneHeight
    {
        Small,
        Medium,
        Large,
        Full
    };
    LaneHeight laneHeight() const { return laneHeight_; }
    void setLaneHeight(LaneHeight height);
    // The lane height buttons (top left, above the deck headers) were clicked.
    std::function<void(LaneHeight)> onLaneHeightChosen;

    Id selectedClip() const { return selected_; }
    void setSelectedClip(Id id);
    std::function<void(Id)> onSelectionChanged;

    // Adds the file to the set; returns the timeline time where the new clip ends.
    std::function<std::optional<double>(const juce::File&, int deck, double seconds)> onFileDropped;

    void zoomIn();
    void zoomOut();
    void zoomToSet();
    void zoomToClip();
    void zoomToTransition();
    // Centres on the playhead, showing two bars of the music playing there.
    void zoomToBeats();

    // The clip on a deck at the given time, or failing that the next one after it, or the
    // last one before it. 0 if the deck is empty.
    Id clipOnDeckNear(int deck, double seconds) const;
    // Clips in start order, for stepping through with the keyboard.
    std::vector<Id> clipsInOrder() const;
    const ClipTiming* timingOf(Id clip) const;
    // Spans where two or more decks play together, in time order.
    std::vector<std::pair<double, double>> mixes() const;

    // Which automation each lane shows, as saved with the set.
    std::array<std::array<bool, kNumParams>, kMaxDecks> shownAutomation() const { return shown_; }
    void setShownAutomation(const std::array<std::array<bool, kNumParams>, kMaxDecks>& shown);
    bool showsAutomation(int deck, Param param) const;
    void setShowsAutomation(int deck, Param param, bool show);
    // Shows every control's automation on the deck, or hides them all if any is showing.
    void toggleAllAutomation(int deck);

    // Brings the playhead into view and resumes following it.
    void showPlayhead();

    // Where glitches happened, marked on the ruler.
    void setGlitchMarks(std::vector<double> seconds);
    // Mixer moves being recorded: each pass is drawn as a plain line (no keypoints) from where it
    // began to the playhead, over the curve it replaces. Call repaint() as it grows.
    void showRecording(const MixRecorder* recorder) { recorder_ = recorder; }

    // Draws the selected clip's bar lines faintly through the other decks' lanes, for lining up a
    // third or fourth record against it.
    void setShowsBeatGuides(bool show)
    {
        beatGuides_ = show;
        repaint();
    }
    bool showsBeatGuides() const { return beatGuides_; }

    void setFollowPlayhead(bool follow) { follow_ = follow; }
    bool followsPlayhead() const { return follow_; }

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    void mouseMagnify(const juce::MouseEvent& e, float scaleFactor) override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragMove(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    bool isInterestedInDragSource(const SourceDetails& details) override;
    void itemDragMove(const SourceDetails& details) override;
    void itemDragExit(const SourceDetails& details) override;
    void itemDropped(const SourceDetails& details) override;

private:
    static constexpr int kHeaderWidth = 158;
    static constexpr int kPitchWidth = 46;      // the pitch fader, at the left of the lane header
    static constexpr int kMinLaneHeight = 44;
    static constexpr int kRulerHeight = 26;
    static constexpr int kEdgeGrab = 6;

    enum class Part
    {
        None,
        Body,
        StartEdge,
        EndEdge
    };
    struct Hit
    {
        Id clip = 0;
        Part part = Part::None;
    };

    enum class Drag
    {
        None,
        Move,
        TrimStart,
        TrimEnd,
        Scrub,
        Keypoint,
        FlatLine // a flat stretch of automation moving up and down as one; a click adds a keypoint
    };

    int numDecks() const { return document_.set().numDecks; }
    juce::Rectangle<int> laneBounds(int deck) const;
    juce::Rectangle<int> waveformBounds(int deck) const;
    // Where automation lines are drawn (below clip title bars); empty when the lane shows none.
    juce::Rectangle<int> automationBounds(int deck) const;
    bool anyAutomationShown(int deck) const;
    int deckAt(int y) const; // -1 outside the lanes
    double timeToX(double t) const { return kHeaderWidth + (t - viewStart_) * pixelsPerSecond_; }
    double xToTime(double x) const { return viewStart_ + (x - kHeaderWidth) / pixelsPerSecond_; }
    double laneWidth() const { return std::max(1, getWidth() - kHeaderWidth); }

    void setView(double start, double pixelsPerSecond);
    void zoomAround(double factor, double anchorTime);
    void zoomToRange(double from, double to);
    Hit clipAt(juce::Point<float> position) const;
    void dropFiles(const juce::Array<juce::File>& files, int x, int y);

    // Automation editing.
    static constexpr int kStartPoint = -1; // keypoint index meaning the starting value
    struct AutomationHit
    {
        Id clip;
        Param param;
        std::optional<int> keypoint; // empty: on the line between keypoints
        int64_t frame;               // where on the line, for adding a keypoint
        double value;
    };
    double valueToY(Param param, double value, juce::Rectangle<int> area) const;
    double yToValue(Param param, double y, juce::Rectangle<int> area) const;
    Id clipAtTime(int deck, double x) const;
    std::vector<juce::Point<float>> automationVertices(const Clip& clip, const ClipTiming& timing, Param param,
                                                       juce::Rectangle<int> area) const;
    std::optional<AutomationHit> automationAt(juce::Point<float> position) const;
    void automationMouseDown(const juce::MouseEvent& e, const AutomationHit& hit);
    void showAutomationMenu(Id clip, Param param, std::optional<int> keypoint);
    // If the automation line at `frame` is flat, the keypoints at its ends (kStartPoint for the
    // starting value); one end if it runs to the end of the clip. Empty on a slope.
    std::optional<std::vector<int>> flatLineEnds(const Clip& clip, Param param, int64_t frame) const;
    void setDownbeatAt(Id clip, float x, bool snapToBeatLine);
    void navigatedByHand() { followSuspended_ = engine_.isPlaying(); }
    bool following() const { return follow_ && !followSuspended_ && engine_.isPlaying(); }

    void paintRuler(juce::Graphics& g);
    void paintClip(juce::Graphics& g, const Clip& clip, const ClipTiming& timing);
    void paintWaveform(juce::Graphics& g, const Clip& clip, const ClipTiming& timing, const WaveformPeaks& peaks,
                       juce::Rectangle<int> area);
    void paintBeats(juce::Graphics& g, const Clip& clip, const ClipTiming& timing, const Track& track,
                    const BeatGrid& grid, juce::Rectangle<int> area);
    // Calls draw(x, beat) for the clip's beat lines between timeline times left and right. Only
    // every beat, bar, 4 bars or 16 bars, whichever is the finest that's far enough apart to read,
    // and never finer than minimumStep beats. Beat 0 is the grid's first downbeat.
    void forEachVisibleBeat(const Clip& clip, const ClipTiming& timing, const Track& track, const BeatGrid& grid,
                            double left, double right, int64_t minimumStep,
                            const std::function<void(float x, int64_t beat)>& draw) const;
    void paintBeatGuides(juce::Graphics& g);
    void paintAutomation(juce::Graphics& g, const Clip& clip, const ClipTiming& timing, Param param,
                         juce::Rectangle<int> area);
    // The set's curve, with its keypoints.
    void paintCurve(juce::Graphics& g, const Clip& clip, const ClipTiming& timing, Param param,
                    juce::Rectangle<int> area);

    SetDocument& document_;
    AudioPool& pool_;
    Engine& engine_;
    const DisplayOptions& display_;

    // Made once: a Font built from options on every paint looks its typeface up again each time,
    // which cost ~10% of a core while scrolling.
    const juce::Font laneFont_{juce::FontOptions(14.0f, juce::Font::bold)};
    const juce::Font rulerFont_{juce::FontOptions(11.0f)};
    const juce::Font titleFont_{juce::FontOptions(12.0f)};

    double pixelsPerSecond_ = 2.0;
    double viewStart_ = -10.0;
    double playhead_ = 0.0;       // as drawn: the engine's position, carried forward between reports
    double lastReported_ = -1.0;  // the engine's last reported position
    double lastReportClock_ = 0.0; // when it was reported (seconds)
    bool follow_ = true;
    bool beatGuides_ = true;
    bool followSuspended_ = false; // the user scrolled away while playing

    Id selected_ = 0;
    std::map<Id, ClipTiming> timings_;
    std::map<Id, Issue::Severity> issues_;
    std::vector<std::pair<double, double>> transitions_; // spans where two decks play
    std::vector<double> glitchMarks_;
    const MixRecorder* recorder_ = nullptr;

    std::array<std::array<bool, kNumParams>, kMaxDecks> shown_{};
    std::array<std::array<juce::ToggleButton, kNumParams>, kMaxDecks> toggles_;
    std::array<std::unique_ptr<PitchFader>, kMaxDecks> pitchFaders_;
    LaneHeight laneHeight_ = LaneHeight::Full;
    std::array<juce::TextButton, 4> laneHeightButtons_;

    Drag drag_ = Drag::None;
    Clip dragOrigin_;
    double dragAnchorTime_ = 0.0;
    std::optional<ClipTiming> dragTiming_; // the clip's timing when a keypoint drag began
    Param dragParam_ = Param::Volume;
    int dragKeypoint_ = 0;
    struct FlatLineDrag
    {
        AutomationHit hit;              // where it was pressed, for a click that adds a keypoint
        std::vector<int> ends;          // keypoint indexes at either end (kStartPoint: starting value)
        double value = 0.0;             // the line's value when pressed
    };
    FlatLineDrag flatLine_;
    std::optional<std::pair<int, double>> dropHint_; // deck, time
};

} // namespace vmp

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
// Deck actions are drawn along the bottom of each clip as curves of the record's speed: pushes
// above the line, held back below it. Stopped, drag one to move it along the music (Shift: finely).
// Drag its number up or down to change how far it moves the music (Shift: finely).
// Double-click one to delete it, right-click for more (a nudge's speed). Right-click a record to
// insert a nudge or a hold where clicked, or to clean up its nudges over a stretch dragged out
// with a rectangle.
//
// Which part of the turntable a mouse gesture touches:
// - Stopped: dragging a record moves it in time or to another deck (turning a stopped record by
//   hand changes what's under the needle when it starts, which is the same thing).
// - Playing: the hand is on the record. A quick click selects it; pressing a moment or dragging
//   takes hold of it, and it follows the mouse (still: held still; left: pushed on, never back),
//   written as a touch. Option-drag moves it instead, as the tone arm would. A record that isn't
//   playing at the moment moves as when stopped.
// - Dragging an edge trims (needle down, needle up), stopped or playing. Shift keeps its meanings.
// - Cmd is the beat grid, not the record: Cmd-click the downbeat, then Cmd-Shift-click another
//   beat (the next one is best), and the grid is fitted to the music's transients around them;
//   the clicks only need to be near. Cmd-drag a beat line to where the beat really is in the
//   waveform (Shift: finely) to pin it. The beats between it
//   and the neighbouring pins spread out to fit; the first pin also pins the downbeat. A pin's
//   handle (top of the waveform) drags without Cmd; double-click or right-click it to unpin.
// - Cmd-drag from a record into another deck's lane links the two moments (on beat lines if
//   near): the record mixed in later keeps them heard together (edit::solveLinks). Drag a link's
//   dot to move that end; double-click or right-click it to delete. Moving a record with the
//   tone arm (Option-drag, stopped or playing) breaks its links.
//
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
    // The timeline time being heard now (the playhead minus the output latency).
    std::function<double()> heardSeconds;
    // "Line up beats here" was chosen on a record (MainComponent::lineUpBeats).
    std::function<void(Id clip, double seconds)> onLineUpBeats;
    // Cmd-click: the downbeat of the track is about here. Cmd-Shift-click: so is another beat,
    // with the last downbeat clicked on the track if there was one. Seconds in the track.
    std::function<void(Id track, double seconds)> onDownbeatClicked;
    std::function<void(Id track, std::optional<double> downbeat, double seconds)> onBeatClicked;

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
    bool keyPressed(const juce::KeyPress& key) override;
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
        FlatLine, // a flat stretch of automation moving up and down as one; a click adds a keypoint
        Touch,    // a hand on a playing record
        Action,       // a nudge or touch dragged along the music (stopped)
        ActionAmount, // its number dragged up or down: how far it moves the music
        BeatPin,      // Cmd: a beat line pinned and dragged to where the beat really is, or into
                      // another lane to link it there
        LinkEnd       // one end of a beat link dragged along its record's music
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
    // Beat pins: the clip's beat line under the mouse (any drawn beat, within a few pixels), and a
    // pin's handle at the top of the waveform.
    struct BeatHit
    {
        Id clip = 0;
        int64_t beat = 0;
    };
    std::optional<BeatHit> beatLineAt(juce::Point<float> position) const;
    std::optional<BeatHit> beatPinAt(juce::Point<float> position) const;
    void showBeatPinMenu(Id clip, int64_t beat);
    // Beat links: drawn as a line between the two moments (upright when kept), a dot at each end.
    struct LinkHit
    {
        Id link = 0;
        Id clip = 0; // the end's record
    };
    std::optional<LinkHit> linkEndAt(juce::Point<float> position) const;
    void paintLinks(juce::Graphics& g);
    void showLinkMenu(Id link, Id clip);
    // The clip's frame at x (by `timing`), on its beat line if one is within a few pixels.
    int64_t snappedFrame(const Clip& clip, const ClipTiming& timing, float x) const;
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
    // The clip's nudges, in the strip along the bottom of its waveform; actionAt() finds one by position.
    void paintActions(juce::Graphics& g, const Clip& clip, const ClipTiming& timing, juce::Rectangle<int> waveArea);
    juce::Rectangle<int> actionArea(const Clip& clip, const ClipTiming& timing, size_t index,
                                   juce::Rectangle<int> waveArea) const;
    std::optional<std::pair<Id, size_t>> actionAt(juce::Point<float> position) const;
    // Where an action's number is drawn (beside its mark `box`), if there's room for it.
    std::optional<juce::Rectangle<int>> actionLabelArea(juce::Rectangle<int> box) const;
    std::optional<std::pair<Id, size_t>> actionLabelAt(juce::Point<float> position) const;
    void showActionMenu(Id clip, size_t index);

    // A hand on a playing record: recorded as a touch from the heard playhead (heardSeconds), written
    // as far as it's gone (and a little ahead) while the mouse is down, and for good, with the
    // records after it following, when let go. A click that never takes hold only selects.
    struct HandOnRecord
    {
        Id clip = 0;
        bool active = false;      // taken hold of (pressed long enough, or moved)
        double pressClock = 0.0;  // when pressed (seconds, UI clock)
        float mouseX = 0.0f;      // where the mouse is
        float handX = 0.0f;       // where it was when it took hold
        int64_t frame = 0;        // where the touch is, in the music
        double start = 0.0;       // heard time it took hold
        double music = 0.0;       // how far the hand has taken the record since (seconds)
        std::vector<SpeedPoint> speeds;
    };
    double heardNow() const;
    bool startTouch();
    void sampleTouch();
    void endTouch();
    std::optional<size_t> touchIndex() const;
    HandOnRecord touch_;

    // Right-click on a record: insert a nudge or hold there, or clean up its nudges.
    void showClipMenu(Id clip, double atSeconds);
    // Cleaning up a record's nudges: after choosing it from the menu, drag a rectangle over the
    // stretch to clean up (edit::tidyActions); Esc or a right-click cancels.
    struct CleanUp
    {
        Id clip = 0;
        bool dragging = false;
        juce::Point<float> from, to;
    };
    std::optional<CleanUp> cleanUp_;
    void paintCleanUp(juce::Graphics& g);
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
    size_t dragAction_ = 0;
    double dragActionStart_ = 0.0; // where the dragged action began, on the timeline
    double dragActionAmount_ = 0.0; // its offset when the drag began
    int64_t dragBeat_ = 0;          // the beat being pinned
    bool dragHasBeat_ = false;      // on a beat line (pinning); else a Cmd-drag can only link
    BeatGrid dragGrid_;             // the track's grid when the drag began
    std::optional<BeatGrid> dragTrackGrid_; // its correction then (none: the detected grid)
    int64_t dragLinkFrame_ = 0;     // where a link would start, in the dragged record
    std::optional<juce::Point<float>> linkTo_; // the mouse, while over another lane
    std::optional<std::pair<Id, double>> downbeatClick_; // the last Cmd-click: track, seconds in it
    Id dragLink_ = 0;
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

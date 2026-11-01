#pragma once

#include "app/AudioPool.h"
#include "app/Display.h"
#include "core/Document.h"
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
// Each lane can show one control's automation in a strip under the waveforms (menu in the lane
// header): click to add a keypoint, drag to move it, double-click to delete, right-click for more.
class TimelineView : public juce::Component, public juce::FileDragAndDropTarget, public juce::DragAndDropTarget
{
public:
    TimelineView(SetDocument& document, AudioPool& pool, Engine& engine, const DisplayOptions& display);

    // The set or loaded audio changed.
    void refresh();
    // Called from the UI timer.
    void updatePlayhead();

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

    std::optional<Param> automationView(int deck) const { return automationView_[static_cast<size_t>(deck)]; }
    void setAutomationView(int deck, std::optional<Param> param);
    // None -> Pitch -> Volume -> Hi -> Mid -> Low -> None.
    void cycleAutomationView(int deck);

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
    static constexpr int kHeaderWidth = 96;
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
        Keypoint
    };

    int numDecks() const { return document_.set().numDecks; }
    juce::Rectangle<int> laneBounds(int deck) const;
    juce::Rectangle<int> waveformBounds(int deck) const; // the lane minus its automation strip
    juce::Rectangle<int> stripBounds(int deck) const;    // empty when no automation is shown
    int deckAt(int y) const; // -1 outside the lanes
    double timeToX(double t) const { return kHeaderWidth + (t - viewStart_) * pixelsPerSecond_; }
    double xToTime(double x) const { return viewStart_ + (x - kHeaderWidth) / pixelsPerSecond_; }
    double laneWidth() const { return std::max(1, getWidth() - kHeaderWidth); }

    void setView(double start, double pixelsPerSecond);
    void zoomAround(double factor, double anchorTime);
    void zoomToRange(double from, double to);
    Hit clipAt(juce::Point<float> position) const;
    void dropFiles(const juce::Array<juce::File>& files, int x, int y);

    // Automation strip editing.
    double valueToY(Param param, double value, juce::Rectangle<int> strip) const;
    double yToValue(Param param, double y, juce::Rectangle<int> strip) const;
    Id clipInStripAt(int deck, double x) const;
    std::optional<size_t> keypointAt(const Clip& clip, Param param, juce::Point<float> position) const;
    void stripMouseDown(const juce::MouseEvent& e, int deck);
    void showStripMenu(Id clip, Param param, std::optional<size_t> keypoint);

    void paintRuler(juce::Graphics& g);
    void paintClip(juce::Graphics& g, const Clip& clip, const ClipTiming& timing);
    void paintWaveform(juce::Graphics& g, const Clip& clip, const ClipTiming& timing, const WaveformPeaks& peaks,
                       juce::Rectangle<int> area);
    void paintBeats(juce::Graphics& g, const Clip& clip, const ClipTiming& timing, const Track& track,
                    const BeatGrid& grid, juce::Rectangle<int> area);
    void paintAutomation(juce::Graphics& g, const Clip& clip, const ClipTiming& timing, Param param,
                         juce::Rectangle<int> area);

    SetDocument& document_;
    AudioPool& pool_;
    Engine& engine_;
    const DisplayOptions& display_;

    double pixelsPerSecond_ = 2.0;
    double viewStart_ = -10.0;
    double playhead_ = 0.0;
    bool follow_ = true;

    Id selected_ = 0;
    std::map<Id, ClipTiming> timings_;
    std::map<Id, Issue::Severity> issues_;
    std::vector<std::pair<double, double>> transitions_; // spans where two decks play

    std::array<std::optional<Param>, kMaxDecks> automationView_;
    std::array<juce::ComboBox, kMaxDecks> viewBoxes_;

    Drag drag_ = Drag::None;
    Clip dragOrigin_;
    double dragAnchorTime_ = 0.0;
    std::optional<ClipTiming> dragTiming_; // the clip's timing when a keypoint drag began
    Param dragParam_ = Param::Volume;
    size_t dragKeypoint_ = 0;
    std::optional<std::pair<int, double>> dropHint_; // deck, time
};

} // namespace vmp

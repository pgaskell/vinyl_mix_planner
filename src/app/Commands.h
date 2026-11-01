#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace vmp
{

// Every keyboard-mappable action. Values are stored in saved key mappings, so append new
// commands at the end rather than renumbering.
enum Command : juce::CommandID
{
    kNew = 1,
    kOpen,
    kSave,
    kSaveAs,
    kUndo,
    kRedo,
    kDelete,
    kPlayPause,
    kGoToStart,
    kZoomIn,
    kZoomOut,
    kZoomSet,
    kZoomClip,
    kZoomTransition,
    kToggleFollow,
    kNudgeEarlier,
    kNudgeLater,
    kNudgeEarlierFine,
    kNudgeLaterFine,
    kPitchUp,
    kPitchDown,
    kAudioSettings,
    kZoomBeats,
    kNudgeBeatEarlier,
    kNudgeBeatLater,
    kPitchUpCoarse,
    kPitchDownCoarse,
    kPitchReset,
    kSelectDeck1,
    kSelectDeck2,
    kSelectDeck3,
    kSelectDeck4,
    kSelectNextClip,
    kSelectPreviousClip,
    kTrimStartToPlayhead,
    kTrimEndToPlayhead,
    kJumpBack,
    kJumpForward,
    kGridDownbeatHere,
    kGridEarlier,
    kGridLater,
    kGridHalve,
    kGridDouble,
    kGridSlower,
    kGridFaster,
    kGridReset,
    kToggleKeyNotation,
    kKeyboardShortcuts,
    kStop,
    kPreviousMix,
    kNextMix,
    kCycleAutomationView,
    kNudgeBarsEarlier,
    kNudgeBarsLater,
    kShowPlayhead,
    kToggleBeatGuides,
    kLastCommand = kToggleBeatGuides
};

} // namespace vmp

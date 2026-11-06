# Personality

You are a pragmatic senior engineer with strong taste.
You optimize for truth, clarity, and usefulness over politeness theater.

## Style
- Be direct without being cold
- Prefer substance over filler
- Push back when something is a bad idea
- Admit uncertainty plainly
- Keep explanations compact unless depth is useful

## What to avoid
- Sycophancy
- Hype language
- Repeating the user's framing if it's wrong
- Overexplaining obvious things

## Technical posture
- Prefer simple systems over clever systems
- Care about operational reality, not idealized architecture
- Treat edge cases as part of the design, not cleanup

# Project: Vinyl Mix Planner

macOS app (Apple Silicon, target Tahoe plus earlier/later versions) for planning vinyl DJ sets
from audio files, constrained to what a Technics SL-1200MK2 can actually do.
2 decks by default, up to 4. Must output to multichannel interfaces (Pioneer DJM-900SRT,
DJM-900NXS2, and generic Core Audio devices).

## Confirmed requirements
- Use cases and the modes they imply (planning, practice, mixing ahead): `docs/USE_CASES.md`.
- Goal: plan almost everything the user would do live on their real setup.
- Two output modes: internal mix (laptop, stereo out) and direct out (each deck dry to its own
  channel pair into a hardware mixer; mixer EQ/volume/FX recorded as automation over USB MIDI).
- DAW-style timeline: per-deck lanes, waveform clips you can move/trim; drag files onto a lane.
  No global tempo grid: tracks aren't perfectly on a grid themselves.
- Simple library to browse files.
- Pitch: one setting per track (±8%, 0.02% steps, 0 = quartz lock), with optional automated pitch
  changes where they help a transition. Pitch shifts key (resampling, never time-stretch).
- 33/45 is only a label; the user often won't know it. No wrong-speed playback initially.
- Mixer is modular so other mixers can be emulated later. Start with a basic mixer
  (EQ + volume per channel), then emulate the DJM-900NXS2: 3-band isolator, volume, and
  Sound Color FX (Space, Dub Echo, Sweep (gate/comp), Filter): one type selected for the whole
  mixer, a COLOR knob per channel. Beat FX are out of scope for now.
- The user's mixers: DJM-900SRT, DJM-900NXS2 (primary); also Warm4, an A&H 4-channel, Omnitronic 202.
- Bounce the set to stereo in several file formats.

## Stack
- C++20, JUCE 9 (AGPLv3), libsamplerate (resampling), nlohmann/json (set files), Catch2 (tests). Submodules in `external/`.
- CMake + Ninja with presets; minimum macOS 11.0; release builds universal.
- License: AGPL-3.0-or-later (forced by JUCE's open-source license).

## Architecture rules
- `src/core` and `src/engine` must not depend on JUCE. They are unit tested and will be reused
  for offline bounce, which must run the same code as live playback.
- Audio thread: no locks, no allocation, no freeing. Controls are atomics; whole objects
  (tracks) go through the pending/retired hand-off in `Deck`.
- Store positions in source-file frames, not seconds after pitch. Timeline is in seconds; no tempo grid.
- Automation: per clip, per control (pitch, volume, hi, mid, low, color). Trim is per clip, not automated. The clip's starting value is a fixed
  point at the needle drop; keypoints (anchored to source frames, i.e. the music) follow, linear between
  points; keypoints before the in-point are ignored. Trimming the start carries automated values into the
  starting values (pitch may become fractional steps). Pitch slides are linear in time (PitchCurve).
  Stopped, moving a mixer control edits the set (`edit::setParamAt`: the starting value, or a keypoint at
  the playhead if it has keypoints). Playing, moves on the clip being heard go to `MixRecorder` instead
  (latched until stop or the clip ends; the engine plays the latched values through its live controls).
  The mode the control was taken hold of in (`AutomationMode`; each pass keeps its own, so switching modes
  while playing never changes what's heard) decides what happens: Override drops the pass (practice), Latch
  writes the positions until playback stops, jumps or the clip ends (`edit::writeRecordedPass`, thinned),
  Trim writes the existing curve adjusted by the amounts from the first move until the pass ends
  (`edit::writeTrimmedPass`), Trim All adjusts the whole clip's curve (starting value and every keypoint)
  by the last amount (`edit::trimAutomation`); both use `MixerChannel::trimmed` (volume and EQ offset in dB,
  so a closed fader and a kill stay put; COLOR's distance from centre multiplied) and the engine plays them
  via setLiveScale.
- `ClipTiming` is the only place that maps timeline <-> source; UI and engine must both use it.
- All set changes go through `SetDocument::edit` with an `edit::` function (undo = whole-set snapshots;
  wrap drags in begin/endGesture). The engine gets an immutable `Schedule` rebuilt after each change.
- Deck actions (`Clip::actions`) are what the DJ did to the record while it played: a lane of their
  own per clip, not pitch automation. Each is a short curve of speed factors (`speedShape`: 1
  untouched, 0 held still) that multiplies the pitch's, anchored to a source frame: a Nudge (from
  the keys: `seconds` and a peak `speed`, in a set hand-like shape) or a Touch (measured from the
  mouse). `ClipTiming` warps pitch time into timeline time with them (held still is a step), so the
  engine and bounce play them; Deck holds its place on an empty range. Only forwards for now:
  backwards (winding back, beat jumps, loops) needs reverse playback and a non-monotone warp.
  Moving a clip or changing its pitch drops its actions (they were played against another record
  at that tempo); being moved or re-pitched as a follower doesn't.
- Mouse modifiers say which part of the turntable is touched (TimelineView's class comment):
  stopped, dragging a record moves it; playing, the hand is on the record (a quick click selects,
  pressing 150 ms or dragging records a touch that follows the mouse), and Option is the tone arm
  (moves it, stopped or playing, and breaks its beat links). Edges trim either way. Shift keeps its meanings (fine drag, set downbeat). New
  gestures should fit this: hand = plain, tone arm = Option, beat grid = Cmd (Cmd-click the downbeat and
  Cmd-Shift-click another beat: `fitDownbeat`/`fitBeatGrid` fit the grid to the transients, the
  clicks only need to be near; Cmd-drag a beat line pins it; Cmd-drag into another lane links the two
  records there). The toolbar's mouse help (`app/MouseHelp.cpp`) lists every gesture: keep it in step.
- Beat grids (`BeatGrid`) are steady from `firstBeatSeconds` at `bpm`, or bent by pins (beats the
  DJ placed where they really are): even between pins, at `bpm` outside them; beat 0 stays the
  downbeat. Anything that needs a beat's position or length goes through secondsOfBeat/beatAt/
  periodAt, never `firstBeatSeconds + k * period`.
- Beat links (`Set::links`, `BeatLink`): two moments, in two records on different decks, heard
  together. The record that started later follows. `SetDocument::edit` runs `edit::solveLinks`
  after every change, so links always win. It places the follower by its first link; with two or more links
  it owns the follower's pitch (starting value and keypoints): one step setting for each run of links
  it keeps within a nudge (50 ms) per gap, the fader moving just after the link where a run ends (a
  tempo change in either record). It writes a nudge (`DeckAction::linked`) before each later link
  for what the steps leave. All of it is something a
  DJ could do on a turntable. Its linked nudges are rewritten each time; hand nudges stay and are
  made up for. A follower's start belongs to its links (`moveClip` keeps it). Links it can't keep
  are reported by `validate()`. Line Up Beats (`edit::lineUpBeats`) makes a link from the grids.
- Nudges and pitch changes made from the UI go through `edit::withFollowers`, so the clips mixed in
  after the edited one (while it plays, and into those in turn) move and re-pitch with it; a record
  after a gap stays put. Timeline drags (arranging) don't.
- Mixer settings are stored as knob/fader positions (0..1), never gains: the mixer model maps them.
- Track audio is decoded once into `~/Library/Caches/org.vinylmixplanner.app/Audio` (float, with
  waveform peaks) and streamed: `engine/Prefetcher` preads chunks into a fixed, mlocked pool around
  the playhead; the audio thread reads only published chunks (`TrackAudio::framesAt`), never a file
  or a mapping. Freed chunks are reused only after `Engine::renderCounter()` shows no render can
  still hold them. The app hands schedules to the prefetcher, never straight to the engine, so a
  schedule's audio is loaded before it plays. If the read margin (`Deck::kReadMarginSeconds`) no
  longer covers what Deck reads, playback reads unloaded chunks: keep them in step.
- Glitches (engine overload, driver dropouts, missing or late audio) go through an SPSC queue to the
  Glitches tab; anything that can make playback drop out should report one.
- Analysis (`core/Analysis`): BPM/beat grid from spectral flux (tempo from full band, beat phase from
  <200 Hz), key from chroma vs Krumhansl-Kessler profiles. Results cached per file in
  `~/Library/Application Support/VinylMixPlanner/analysis.json`; bump `kAnalyserVersion` when the
  detectors change. The DJ's corrections live on `Track` in the set and override the cache.
  The onset timing constant was calibrated on synthetic kicks only.
- Deck slews to small position changes (<100 ms, +/-10% speed) instead of jumping, so live nudges and
  pitch changes (which pivot on the playhead) sound like a real platter.
- `Command` ids (app/Commands.h) are stored in saved key mappings: append, never renumber.
- Backwards compatibility is for the developer's own sets and settings only, until a public release.
  Any shim that keeps old files or settings working must be listed in TODO.md under
  "Before a public release: drop backwards compatibility", so it can be removed in one go.
- Drivers only pass enabled channels: always route through the physical->buffer map.
- Hardware mixers: `core/HardwareMixer` holds each model's MIDI map (`MixerProfile`, matched by device
  name) and follows its controls. MIDI arrives as 0..127; knobs map with the centre detent exactly at 0.5.
  The DJM's control MIDI is off until its ON/OFF (UTILITY) button is pressed; selectors are tracked by
  the last note-on (offs can go missing). USB pair n plays on channel n. A turn of a TRIM knob sets the trim of
  its deck's record (the one heard, else the next one), one undo step per turn; it's never a live control. Reading Core Audio inputs
  (any device, not just the built-in mic) needs microphone permission, or they read as silence.

## Building on this machine
- Use `/opt/homebrew/bin/cmake` and `ninja`. The ones in `/usr/local/bin` are Intel builds that
  would run clang under Rosetta.
- Active toolchain is Xcode 27.2 (`xcode-select`). The standalone Command Line Tools have a
  MacOSX27 SDK their linker can't read; don't switch back to them.
- `cmake --preset debug && cmake --build --preset debug && ctest --preset debug`

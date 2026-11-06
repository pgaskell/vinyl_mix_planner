# To do

## Beat grid

- **Beat links and pins, next steps.**
  - **Done:** Beat pins (`BeatGrid::pins`, Cmd-click/drag a beat line). Beat links: Cmd-drag from
    one record into another lane; Line Up Beats (M) makes one from the grids. `edit::solveLinks`
    keeps them with placement, pitch settings (moving the fader at a tempo change) and nudges.
  - **Hand edits inside a linked stretch** are overruled silently:
    - A nudge is made up for at the next link.
    - A pitch change on a follower with two or more links is undone, and its pitch automation is
      replaced by the links'.
    - Moving a follower is refused.

    The agreed behaviour was to ask whether to remove the link. Do that at the UI entry points
    (nudge keys, touches, pitch fader, clip drag).
  - **Gradual tempo drift** (a live drummer) is kept with steps of the fader and nudges. A linear
    ride between links would follow it more smoothly; consider it if the steps sound busy.
  - **Snapping:** snap link ends and dragged pins to the onset under the mouse (the kick), not
    only to beat lines.
  - **Lock a whole mix:** link every beat (or every bar) through an overlap from the grids. Then
    thin the links to the fewest that keep the drift under a few ms (see Tidy up nudges).
  - **Find pins automatically** where the detected beat phase jumps (a splice) or drifts (a live
    drummer, wow), proposed for the DJ to accept.
  - **Pins live in the set, not with the track,** like every grid correction: see the library item.
- **Bar resets within a track.** Many records are built in 4-bar phrases but slip in an extra bar
  or two (a bridge, a break), after which the downbeat count is off for the rest of the record.
  Let the DJ mark "a new bar 1 starts here" at points in the track: the beats themselves stay on
  the same grid, only bar and phrase counting restart from that beat. Stored on the track with the
  other grid corrections (shift-click currently sets the downbeat for the whole track; this would
  be shift-click with a modifier, or a command at the playhead). Half-bar inserts (2 beats) work
  the same way.

## Automation

- **Recording, next steps.** Mouse moves are recorded or trimmed (Override/Latch/Trim/Trim All button,
  `core/MixRecorder`, `edit::writeRecordedPass`/`writeTrimmedPass`/`trimAutomation`): latched, thinned, placed at the
  playhead minus the output latency.
  - Write mode, with MIDI input: every control of the hardware mixer written from the start of
    playback, touched or not.
  - Trim: check the amounts per turn (24 dB for volume and EQ, 4 octaves of COLOR scale) feel right; maybe
    double-click to reset a trim to none while playing.
  - Feed the DJM's MIDI moves into the same recorder (see "Record mixer moves" below).
  - Tune `kRecordingTolerance` (1% of the range) by eye on real takes.
  - Pitch moves aren't recorded; pitch changes while playing still go through `withFollowers`.
- **Deck actions: capture everything the DJ does to a record while it plays.** The aim: any move
  that changes where the record is (nudges, touches, and for digital decks beat jumps, loops, hot
  cue jumps, reverse, brake) is written when done while playing and editable afterwards, like the
  mixer automation. Done so far: `Clip::actions` as speed curves on top of the pitch: Nudge from
  the keys (a set shape: in quickly, then easing off) and Touch measured from the mouse (press a
  playing record and it follows the hand). Drawn along the bottom of the clip; double-click
  deletes one; right-click clears them or sets a nudge's peak speed. Still to do:
  - Winding back: reverse playback in Deck and a warp that can go backwards (also what beat jumps
    and loops need). Today the hand can only hold the record or push it on.
  - Edit a curve by hand (drag its points), resize a nudge (`edit::setActionSeconds`).
  - Thin recorded touches (they keep every UI tick, ~60 a second); thinning the speed curve
    changes where the music ends up, so thin the position and refit, or correct the offset.
  - Tune the nudge shape and the 30 ms let-go from measured touches.
  - **Tidy up nudges** on a selected stretch of time (undoable, never automatic). The usual case:
    the DJ played the transition, heard the beats drift apart late and corrected late, maybe the
    wrong way first. Rework the nudges and touches in the selection into fewer, earlier ones that
    end with the music in the same place:
    - Earlier: move the correction towards the start of the selection, before the drift was heard,
      making up for the DJ's reaction time; ideally where the drift starts, so it never shows.
    - Cancel out: a nudge the wrong way followed by corrections the other way (push, push, drag,
      drag, hold) becomes the one net move, or nothing if they cancel.
    - Merge: several small nudges the same way close together become one.
    - Hide: within that, prefer where it's least noticeable, e.g. a breakdown, a quiet stretch, or
      under the other record while it's louder; and keep it off the beats the ear locks onto.
    - Keep the alignment: it has to leave the two records' beats at least as close as before
      through the selection, not just the end position. Check against both beat grids; flag where
      it can't (a drift that needs a pitch change, not a nudge).
    - Needs a time selection on the timeline first; show the before and after (ghosted) before
      applying.
  - Override (practice) mode doesn't apply: actions are always written. Hearing one without
    writing it needs a live offset in the engine.
  - Nudges and touches from a hardware platter or jog over MIDI, into the same lane.
  - Pitch fader and pitch bend moves recorded as a pass, like `writeRecordedPass`.

## Mixer and decks UI

- **Undoable zooms.** Zooming (and the zoom-to commands) should be easy to step back from. View
  changes aren't set edits, so they shouldn't go through `SetDocument` or mark the set unsaved:
  a separate view history (zoom and scroll position), stepped back with Cmd+Z only when the last
  thing done was a zoom, or with its own keys (back/forward, like a browser). Decide which; merging
  a burst of scroll-wheel zooms into one step either way.
- **Trim, next steps.** Trim is in (per clip, follows the DJM's TRIM knobs). The basic mixer maps
  the knob to -20..-4 dB, -12 dB at the centre, so faders can sit fully up (as most Pioneer DJs
  play) without the internal mix clipping.
  - The DJM's own trim runs from off to +9 dB. Measure the DJM's law when emulating it, and keep
    the headroom somewhere (a master level) so the emulation doesn't clip where the basic mixer
    doesn't.
  - Automate it, if it turns out to be needed.
  - Suggest a trim from each track's loudness, measured during analysis.
- **Clipping marks on the ruler**, like the glitch marks: where the internal mix went past full
  scale while playing (the engine reports the mix peak per block; record the position too), and
  where an export clipped (`BounceResult` counts clipped samples; keep their positions, merged
  into stretches). Click one to jump there, a little before. Clear them with the clip light.
- **Mixer that looks like a mixer.**
  - Strip order as on the DJM, top to bottom: trim, HI, MID, LOW, COLOR, fader (in place now).
  - Bigger EQ knobs, drawn as real knobs (a cap, a pointer, the centre detent marked) in a
    LookAndFeel, rather than the default JUCE rotary.
  - The pitch fader is beside each lane now (`PitchFader`); at the Small lane height it's short.
    If that's a problem, a horizontal fader or a number to drag there.
  - The blue hardware marks have to keep working on the new controls.
- **Deck info to the right of each lane** (to be clarified): what the deck is playing at the
  playhead: title, playing BPM, key after pitch, pitch %, time remaining.
- **Keyboard selection of tracks** (to be clarified). Decks 1–4 and next/previous clip already
  have keys.

## Library and playlists

- **Proper library and playlist management**, replacing the plain folder browser:
  - A track list with columns: title, artist, album/label, length, BPM, key (standard or
    Camelot), pressed speed, date added, and how often it's been used in sets. Sortable and
    searchable.
  - Playlists (crates) you can build, reorder and drag into a set.
  - Remember each track's analysis and corrections across sets: BPM, beat grid and
    downbeat, key, and a start point (where you usually drop the needle). Detected BPM and key
    are already cached per file in `analysis.json`; corrections currently live only in the
    set they were made in, and there are no start points yet.
  - Filters for mixing: tracks within a BPM range of the selected track, or in a compatible key.
  - Cope with files that move or go missing (relink).

## Rebuilding a recorded mix

- **Lay out a set from a recording of it.** Given a recorded mix and the vinyl rips (or the same
  tracks) that went into it, place each track on the timeline where it plays in the recording,
  roughly: the DJ will beat-match it again anyway. Find each track by cross-correlating its audio
  (or its spectral flux / chroma) against the recording over a range of speeds; the best-matching
  speed is the pitch it was played at (resampling: tempo and key move together, so one ratio
  explains both). Alternate the decks for overlapping tracks. Hard parts: EQ and filters in the
  transitions, and records that are only heard for a few bars.

## Cueing

- **The cuehead: a second position for preparing ahead.** Independent of the playhead and usually
  ahead of it; a second person (or the DJ between transitions) lines up and auditions what comes
  next while the playhead plays the set to the mixer. See `docs/USE_CASES.md`. Needs:
  - A second engine rendering the same schedule (internal mix) to its own device, typically the
    laptop's built-in output.
  - Its own transport and position on the timeline, drawn differently from the playhead.
  - The prefetcher covering both positions, with the playhead taking priority.
  - Edits locked just ahead of the playhead.
  - A choice of what it plays: just the incoming deck, or the whole mix at that point.

## Digital decks (vinyl mode stays)

- **A deck model per set, like the mixer model.** Vinyl (today: 1200 MK2) stays the default and
  is never watered down. A digital model (CDJ/controller-like) lifts its limits; the set says
  which it's planned for, and switching a set from digital back to vinyl lists, as issues, what
  the turntables can't do. Engine and bounce run the same code for both.
  - What vinyl enforces that digital would relax: pitch range (±8%, then ±16/50%), resampling only
    (key lock needs a time-stretcher: Signalsmith Stretch is MIT; Rubber Band is GPL, fine with
    AGPL), record changeover time, one copy of a record per deck at a time, motor start/stop.
  - Digital features, roughly by value: key lock (master tempo); sync / tempo match to a deck;
    hot cues and loops (on the clip, anchored to source frames like keypoints); beat jump; quantised
    nudges and cue points; instant start; reverse. Each one is a clip feature the timeline can draw
    and the validator can flag in vinyl mode.
  - Open: whether "vinyl" is per set or per deck (a hybrid setup: two 1200s and a CDJ).

## Already planned

- DJM-900NXS2 emulation: isolator curves and the Color FX (Space, Dub Echo, Sweep, Filter),
  with a COLOR knob per channel. The COLOR knob is in (stored, automated, follows the DJM) but is
  only a basic resonant filter (`MixerChannel::colorFilter`): add the FX type selector, measure
  the DJM's Filter, then the other three.
- Record mixer moves (faders, EQ, FX) over MIDI into the automation. The DJM-900SRT is followed
  already (`core/HardwareMixer`, shown as blue marks on the strips in direct out). Still to do:
  - Arm recording; start from the mixer's state (snapshot: hold START/STOP), place each move at the
    playhead minus the output latency, replace the keypoints a gesture covers, then thin them.
  - Fader, trim, HI/MID/LOW and COLOR are in the model now; the crossfader isn't.
  - DJM-900NXS2 profile (from its List of MIDI messages v1.00; same CCs as the SRT, plus USB B and
    RETURN AUX selector positions). Works on a real one (tried briefly, 2026-11-05). Still open:
    - Whether holding START/STOP sends a snapshot (`snapshotKnown` is false until then).
    - USB pair n plays on channel n with the selector at the port's letter (A or B), and which
      port this Mac is on: can the app tell? Then a channel on the other letter is a warning.
    - Its sound color FX buttons (CC 0x69 SPACE, 0x6B DUB ECHO, 0x6A SWEEP, 0x57 FILTER, 0x55
      NOISE, 0x56 CRUSH) aren't followed yet: needed with the FX type selector.
- Record what the mixer sends back over USB (MIX/REC OUT or post-fader channels, chosen in the
  DJM Setup app) as a reference take on the timeline, to compare with the internal mix and check
  the emulation. Needs the input channels opened (the microphone permission is declared).
- Export, next steps. Export Mix (File menu) writes WAV/AIFF/FLAC (16-bit dithered or 24-bit)
  and Ogg Vorbis, of the whole set.
  - AAC/M4A: JUCE can't write it, but macOS's `afconvert` can (bounce to a temporary WAV, convert).
  - MP3 needs an encoder (LAME, LGPL) built in; check it fits the AGPL distribution first.
  - Export a part of the set (between two markers, or the selected mix) to share one transition.
  - Stems: each deck after its channel, one file per deck.
- Sign and notarise the app so it opens without the security warning.

## Before a public release: drop backwards compatibility

Shims that exist only so sets and settings made during development keep working. Remove them
(and their tests) when other people start using the app, and start the set format again at 1.

- `core/SetFile.cpp`: loading version 1 sets (no automation; an unused `"pitchMoves"` field is
  ignored). Test: "Version 1 sets still open".
- `core/SetFile.cpp`: sets from before version 3 have pitch in 0.05% steps and no
  `"pitchStepPercent"`; they're converted on load (×2.5). Keep writing `"pitchStepPercent"`.
  Test: "Version 2 sets keep their pitch exactly…".
- `core/SetFile.cpp`: clips saved before trim and COLOR have no `"trim"`/`"color"` in `"mixer"`;
  they load as 0.5. Test: "Version 1 sets still open" (checks the defaults).
- `app/Commands.h`: command ids are append-only because saved key mappings store them. Renumber
  into a tidy order once, then freeze.
- Settings live in `~/Library/Application Support/VinylMixPlanner`, the decode cache under the
  bundle id (`org.vinylmixplanner.app`). Move settings to the bundle id too, without migrating.

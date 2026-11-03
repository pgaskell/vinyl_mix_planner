# To do

## Beat grid

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

## Mixer and decks UI

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
  - A DJM-900NXS2 profile: run the MIDI probe on it for its map.
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

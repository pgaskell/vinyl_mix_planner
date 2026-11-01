# To do

## Interface

- **Icons on common buttons.** Use the usual symbols instead of text where there is one:
  transport (previous mix, rewind, play/pause, stop, fast forward, next mix), zoom in/out,
  follow, nudge, and pitch up/down/reset. Keep text where no symbol is widely understood
  (Mix, Clip, Transition, Beats), and keep the shortcut in each button's tooltip.

- **VU meters.** A level meter per deck channel and for the master output, with peak hold and
  a clip light, so you can match levels through a transition and see when the mix overloads.

- **Resizable library panel.** Drag the edge between the folder list and the timeline to widen
  it and see long file names; remember the width.
- **Beat lines across decks.** Optionally draw one record's beat lines (bars, 4-bar and 16-bar
  marks) faintly through the other lanes, so lining up a third or fourth deck against it is easy
  by eye. Probably follows the selected clip, with a toggle.

## Beat grid

- **Bar resets within a track.** Many records are built in 4-bar phrases but slip in an extra bar
  or two (a bridge, a break), after which the downbeat count is off for the rest of the record.
  Let the DJ mark "a new bar 1 starts here" at points in the track: the beats themselves stay on
  the same grid, only bar and phrase counting restart from that beat. Stored on the track with the
  other grid corrections (shift-click currently sets the downbeat for the whole track; this would
  be shift-click with a modifier, or a command at the playhead). Half-bar inserts (2 beats) work
  the same way.

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

- **A cue playhead for listening ahead.** A second, separate playhead that plays into the
  headphones while the main playhead plays the mix, so you can audition the next transition
  without stopping. Needs:
  - A headphone output: a second stereo pair on the audio interface (or the mixer's cue
    channel in direct-out mode).
  - Its own transport and position on the timeline, drawn differently from the main playhead.
  - A choice of what it plays: just the incoming deck, or the whole mix at that point.
  - Optionally a cue/master blend for the headphones, as on a DJ mixer.

## Already planned

- DJM-900NXS2 emulation: isolator curves and the Color FX (Space, Dub Echo, Sweep, Filter),
  with a COLOR knob per channel.
- Record mixer moves (faders, EQ, FX) over MIDI into the automation.
- Export the mix to an audio file (WAV, AIFF, FLAC).
- Decode cache: a setting for its size limit (fixed at 10 GB) and a button to clear it.
- Sign and notarise the app so it opens without the security warning.

## Before a public release: drop backwards compatibility

Shims that exist only so sets and settings made during development keep working. Remove them
(and their tests) when other people start using the app, and start the set format again at 1.

- `core/SetFile.cpp`: loading version 1 sets (no automation; an unused `"pitchMoves"` field is
  ignored). Test: "Version 1 sets still open".
- `core/SetFile.cpp`: sets from before version 3 have pitch in 0.05% steps and no
  `"pitchStepPercent"`; they're converted on load (×2.5). Keep writing `"pitchStepPercent"`.
  Test: "Version 2 sets keep their pitch exactly…".
- `app/Commands.h`: command ids are append-only because saved key mappings store them. Renumber
  into a tidy order once, then freeze.
- Settings live in `~/Library/Application Support/VinylMixPlanner`, the decode cache under the
  bundle id (`org.vinylmixplanner.app`). Move settings to the bundle id too, without migrating.

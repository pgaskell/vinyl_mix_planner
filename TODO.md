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

- **Record moves made with the mouse.** Today a control only writes a keypoint at the playhead if
  it already has keypoints. Add a record (arm) button. While it's armed and playing, dragging the
  volume slider or an EQ knob writes keypoints, touch-style:
  - while the control is held, its moves replace the keypoints already there;
  - on release, the control returns to the existing curve;
  - the moves are thinned afterwards.
  
  Build it as one recorder with two inputs, the mouse and hardware MIDI (see "Record mixer moves"
  below), so arming, replacing and thinning are written once.

## Mixer and decks UI

- **Trim, next steps.** Trim is in (per clip, -9 to +9 dB, follows the DJM's TRIM knobs).
  - The DJM's own trim runs from off to +9 dB; the basic mixer maps the knob to -9..+9. Measure
    the DJM's law when emulating it.
  - Automate it, if it turns out to be needed.
  - Suggest a trim from each track's loudness, measured during analysis.
- **Mixer that looks like a mixer.**
  - Strip order as on the DJM, top to bottom: trim, HI, MID, LOW, COLOR, fader (in place now).
  - Bigger EQ knobs, drawn as real knobs (a cap, a pointer, the centre detent marked) in a
    LookAndFeel, rather than the default JUCE rotary.
  - Move the pitch out of the channel strip. It's a turntable control, not a mixer control, and it
    is easy to confuse with the volume fader beside it at the same size. It could go:
    - beside each deck's lane;
    - in deck panels either side of the mixer, like turntables flanking it.

    Keep it long and vertical, + at the bottom, like a 1200.
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

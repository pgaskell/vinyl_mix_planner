# To do

## Interface

- **Icons on common buttons.** Use the usual symbols instead of text where there is one:
  transport (previous mix, rewind, play/pause, stop, fast forward, next mix), zoom in/out,
  follow, nudge, and pitch up/down/reset. Keep text where no symbol is widely understood
  (Mix, Clip, Transition, Beats), and keep the shortcut in each button's tooltip.

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
- Stream audio from disk so long sets don't hold every track in memory.
- Sign and notarise the app so it opens without the security warning.

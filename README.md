# Vinyl Mix Planner

Plan vinyl DJ sets on your laptop before you play them on real turntables.

![A two-deck mix laid out on the timeline, with volume and low-EQ automation](docs/images/Vinyl_Mix_Planner.png)

Load the tracks you own on vinyl as audio files, lay them out on two to four decks, and work
out the mix: where each record drops in, how far to move the pitch fader, and what to do with
the EQ and volume through each transition. The app only lets you do what a Technics SL-1200MK2
can do: ±8% pitch, with tempo and key moving together like a real record. A mix that works
here will work on the decks.

![Zoomed in on a transition: beat lines on both decks show the records in phase](docs/images/Vinyl_Mix_Planner_Close.png)

## What it does

- A timeline with a lane per deck, 3-band waveforms and beat lines for matching by eye
- Pitch, nudge and EQ by ear while the mix plays, the way you would on the decks
- Tempo and key detection, with the key shown after pitch in standard or Camelot notation
- Automation for pitch, volume and EQ through each transition
- Plays through your laptop, or one deck per channel into a club mixer (e.g. Pioneer DJM-900)
- Checks for things you can't do live, like two records on one deck or no time to change records

Still to come: DJM-900 mixer and effects emulation, recording mixer moves over MIDI, and
exporting the mix to an audio file.

## Building

macOS on Apple Silicon, with Xcode, CMake and Ninja installed:

```sh
git clone --recursive https://github.com/pgaskell/vinyl_mix_planner.git
cd vinyl_mix_planner
cmake --preset release && cmake --build --preset release
```

The app is built to `build/release/VinylMixPlanner_artefacts/Release/`.

## License

AGPL-3.0. Built with [JUCE](https://juce.com).

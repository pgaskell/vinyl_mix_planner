# Vinyl Mix Planner

Plan vinyl DJ sets from audio files, limited to what a Technics SL-1200MK2 can actually do:
±8% pitch on a 0.05% grid, with tempo and key moving together (no time-stretching).
Up to 4 decks. Plays through the laptop's own output, or one stereo pair per deck into a
hardware mixer (DJM-900NXS2 / DJM-900SRT or any multichannel Core Audio interface).

**Status:** early but usable. Timeline with one lane per deck: drop files from Finder or the
library, move and trim clips, set each clip's starting pitch, fader and EQ, play back through
the laptop or a hardware mixer, save and open sets, undo. No beat detection, pitch-move editing,
mixer automation, effects or bounce yet.

## Building (macOS, Apple Silicon)

Requirements: Xcode or the Command Line Tools, CMake ≥ 3.22, Ninja.

```sh
git clone --recursive <repo-url>
cd vinyl_mix_planner
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
open "build/debug/VinylMixPlanner_artefacts/Debug/Vinyl Mix Planner.app"
```

If you already cloned without `--recursive`: `git submodule update --init --recursive`.

The `release` preset builds a universal (arm64 + x86_64) binary. The minimum macOS version is 11.

### Machine-specific toolchain fixes

Put local overrides in `CMakeUserPresets.json`, which git ignores. For example, if the
Command Line Tools' newest SDK doesn't match their linker, point at an older SDK:

```json
{
  "version": 6,
  "configurePresets": [{
    "name": "local-debug", "inherits": "debug", "binaryDir": "${sourceDir}/build/debug",
    "environment": { "SDKROOT": "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk" }
  }],
  "buildPresets": [{ "name": "local-debug", "configurePreset": "local-debug" }],
  "testPresets": [{ "name": "local-debug", "configurePreset": "local-debug" }]
}
```

Use `SDKROOT` rather than `CMAKE_OSX_SYSROOT`: JUCE configures its `juceaide` helper as a
separate CMake project, which doesn't inherit cache variables.

## Layout

| Path | What |
|---|---|
| `src/core/` | Set model, timeline/source timing math, edits with undo, validation against what 1200s can do, waveform peaks, `.vmpset` file format. No JUCE. |
| `src/engine/` | Real-time engine: plays the set's schedule through decks (resampling) and a basic mixer, routes outputs. No JUCE. |
| `src/app/` | JUCE application: UI, device and MIDI handling, file decoding. |
| `tests/` | Catch2 unit tests for core and engine. |
| `external/` | Submodules: JUCE 9.0.4, libsamplerate 0.2.2, nlohmann/json 3.12.0, Catch2 3.16.1. |

## License

AGPL-3.0-or-later (see `LICENSE`), which matches JUCE's open-source license.

# Vinyl Mix Planner

Plan vinyl DJ sets from audio files, limited to what a Technics SL-1200MK2 can actually do:
±8% pitch on a 0.05% grid, with tempo and key moving together (no time-stretching).
Up to 4 decks. Plays through the laptop's own output, or one stereo pair per deck into a
hardware mixer (DJM-900NXS2 / DJM-900SRT or any multichannel Core Audio interface).

**Status:** phase 1 (set data model, no UI yet) on top of the phase 0 prototype. Decks load files, play with pitch, motor start/stop and routing
in both output modes, plus a MIDI monitor for the mixer. No timeline, library, EQ or effects yet.

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
| `src/core/` | Set model (tracks, clips, pitch moves), timeline/source timing math, validation against what 1200s can do, `.vmpset` JSON file format. No JUCE. |
| `src/engine/` | Real-time audio engine: decks, resampling, routing. No JUCE, fully unit tested. |
| `src/app/` | JUCE application: UI, device and MIDI handling, file decoding. |
| `tests/` | Catch2 unit tests for core and engine. |
| `external/` | Submodules: JUCE 9.0.4, libsamplerate 0.2.2, nlohmann/json 3.12.0, Catch2 3.16.1. |

## License

AGPL-3.0-or-later (see `LICENSE`), which matches JUCE's open-source license.

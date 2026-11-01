# Developing

macOS, Apple Silicon. Needs Xcode, CMake 3.22 or later, and Ninja.

```sh
git clone --recursive https://github.com/pgaskell/vinyl_mix_planner.git   # or: git submodule update --init --recursive
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
open "build/debug/VinylMixPlanner_artefacts/Debug/Vinyl Mix Planner.app"
```

The `release` preset builds a universal (arm64 + x86_64) app. The minimum macOS version is 11.

| Path | What |
|---|---|
| `src/core/` | Set model, timing maths, automation, edits with undo, analysis, set file format, decode cache files. No JUCE. |
| `src/engine/` | Real-time playback: decks, resampling, basic mixer, output routing, streaming from the decode cache, glitch reports. No JUCE. |
| `src/app/` | JUCE application: UI, audio and MIDI devices, file decoding. |
| `tests/` | Catch2 unit tests for core and engine. |
| `external/` | Submodules: JUCE, libsamplerate, nlohmann/json, Catch2. |

Machine-specific CMake overrides go in `CMakeUserPresets.json`, which git ignores. If you need
a different SDK, set `SDKROOT` in the preset's `environment` rather than `CMAKE_OSX_SYSROOT`:
JUCE configures its `juceaide` helper as a separate CMake project, which doesn't inherit
cache variables.

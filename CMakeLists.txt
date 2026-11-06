cmake_minimum_required(VERSION 3.22)

# Big Sur is the oldest macOS that runs on Apple Silicon.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

project(VinylMixPlanner VERSION 0.6.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VMP_BUILD_TESTS "Build unit tests" ON)

# --- Third-party ------------------------------------------------------------

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(LIBSAMPLERATE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LIBSAMPLERATE_INSTALL OFF CACHE BOOL "" FORCE)
# libsamplerate 0.2.2 declares a CMake minimum that CMake 4 no longer accepts.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory(external/libsamplerate EXCLUDE_FROM_ALL)
unset(CMAKE_POLICY_VERSION_MINIMUM)

set(JSON_Install OFF CACHE BOOL "" FORCE)
add_subdirectory(external/json EXCLUDE_FROM_ALL)

add_subdirectory(external/JUCE EXCLUDE_FROM_ALL)

set(VMP_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)

# --- Core: set model, timing math, file format; no JUCE dependency ----------

add_library(vmp_core STATIC
    src/core/Analysis.cpp
    src/core/Document.cpp
    src/core/Edits.cpp
    src/core/HardwareMixer.cpp
    src/core/MixRecorder.cpp
    src/core/Musical.cpp
    src/core/PitchCurve.cpp
    src/core/Set.cpp
    src/core/SetFile.cpp
    src/core/StreamedAudio.cpp
    src/core/Waveform.cpp)
target_include_directories(vmp_core PUBLIC src)
target_link_libraries(vmp_core PRIVATE nlohmann_json::nlohmann_json)
target_compile_options(vmp_core PRIVATE ${VMP_WARNINGS})
# Track analysis is heavy number crunching; keep it fast in debug builds too.
set_source_files_properties(src/core/Analysis.cpp src/core/Waveform.cpp PROPERTIES COMPILE_OPTIONS -O2)

# --- Engine: real-time audio, no JUCE dependency ----------------------------

add_library(vmp_engine STATIC
    src/engine/Bounce.cpp
    src/engine/Deck.cpp
    src/engine/Engine.cpp
    src/engine/MixerChannel.cpp
    src/engine/Prefetcher.cpp
    src/engine/Schedule.cpp)
target_link_libraries(vmp_engine PUBLIC vmp_core SampleRate::samplerate)
target_compile_options(vmp_engine PRIVATE ${VMP_WARNINGS})

# --- App --------------------------------------------------------------------

juce_add_gui_app(VinylMixPlanner
    PRODUCT_NAME "Vinyl Mix Planner"
    COMPANY_NAME "Vinyl Mix Planner"
    BUNDLE_ID org.vinylmixplanner.app
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/resources/AppIcon.png"
    # Core Audio inputs (a mixer's USB send, not just the built-in mic) need this permission.
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Recording what your mixer sends back over USB."
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

target_sources(VinylMixPlanner PRIVATE
    src/app/AnalysisCache.cpp
    src/app/AudioPool.cpp
    src/app/DriverDropouts.cpp
    src/app/Faders.cpp
    src/app/GlitchPanel.cpp
    src/app/IconButton.cpp
    src/app/Inspector.cpp
    src/app/IssuesPanel.cpp
    src/app/LevelMeter.cpp
    src/app/LibraryPanel.cpp
    src/app/Main.cpp
    src/app/MainComponent.cpp
    src/app/MidiMonitor.cpp
    src/app/MouseHelp.cpp
    src/app/MixExporter.cpp
    src/app/MixerPanel.cpp
    src/app/ProblemReport.cpp
    src/app/TimelineView.cpp)

target_compile_definitions(VinylMixPlanner PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:VinylMixPlanner,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:VinylMixPlanner,JUCE_VERSION>")

target_link_libraries(VinylMixPlanner
    PRIVATE
        vmp_engine
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_audio_devices
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# --- Tests ------------------------------------------------------------------

if(VMP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(external/Catch2 EXCLUDE_FROM_ALL)
    list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/external/Catch2/extras)
    include(Catch)

    add_executable(vmp_tests
        tests/AnalysisTests.cpp
        tests/BounceTests.cpp
        tests/DeckTests.cpp
        tests/EditsTests.cpp
        tests/EngineTests.cpp
        tests/HardwareMixerTests.cpp
        tests/MixerChannelTests.cpp
        tests/MotorTests.cpp
        tests/MusicalTests.cpp
        tests/NudgeTests.cpp
        tests/PitchCurveTests.cpp
        tests/PitchTests.cpp
        tests/RecordingTests.cpp
        tests/SetFileTests.cpp
        tests/SetTests.cpp
        tests/StreamingTests.cpp
        tests/WaveformTests.cpp)
    target_link_libraries(vmp_tests PRIVATE vmp_engine Catch2::Catch2WithMain)
    target_compile_options(vmp_tests PRIVATE ${VMP_WARNINGS})
    catch_discover_tests(vmp_tests)
endif()

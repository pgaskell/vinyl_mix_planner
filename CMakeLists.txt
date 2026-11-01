cmake_minimum_required(VERSION 3.22)

# Big Sur is the oldest macOS that runs on Apple Silicon.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

project(VinylMixPlanner VERSION 0.1.0 LANGUAGES C CXX)

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

add_subdirectory(external/JUCE EXCLUDE_FROM_ALL)

set(VMP_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)

# --- Engine: real-time audio, no JUCE dependency ----------------------------

add_library(vmp_engine STATIC
    src/engine/Deck.cpp
    src/engine/Engine.cpp)
target_include_directories(vmp_engine PUBLIC src)
target_link_libraries(vmp_engine PUBLIC SampleRate::samplerate)
target_compile_options(vmp_engine PRIVATE ${VMP_WARNINGS})

# --- App --------------------------------------------------------------------

juce_add_gui_app(VinylMixPlanner
    PRODUCT_NAME "Vinyl Mix Planner"
    COMPANY_NAME "Vinyl Mix Planner"
    BUNDLE_ID org.vinylmixplanner.app
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

target_sources(VinylMixPlanner PRIVATE
    src/app/Main.cpp
    src/app/MainComponent.cpp
    src/app/DeckStrip.cpp
    src/app/TrackLoader.cpp)

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
        tests/PitchTests.cpp
        tests/MotorTests.cpp
        tests/DeckTests.cpp
        tests/EngineTests.cpp)
    target_link_libraries(vmp_tests PRIVATE vmp_engine Catch2::Catch2WithMain)
    target_compile_options(vmp_tests PRIVATE ${VMP_WARNINGS})
    catch_discover_tests(vmp_tests)
endif()

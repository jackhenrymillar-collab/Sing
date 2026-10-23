cmake_minimum_required(VERSION 3.22)

# Universal binary and minimum macOS version must be set before project().
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build architectures")
    endif()
endif()

project(Sing VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SING_BUILD_PLUGIN "Build the JUCE plugin (VST3, AU, Standalone)" ON)
option(SING_BUILD_TESTS "Build the DSP unit tests" ON)
option(SING_BUILD_TOOLS "Build the render tool (writes demo WAVs, development only)" OFF)
option(SING_BUILD_SNAPSHOT "Build a tool that renders the panel to PNG (development only)" OFF)

include(FetchContent)

# ---------------------------------------------------------------------------
# DSP library: plain C++, no JUCE, so it can be tested on any platform.
# ---------------------------------------------------------------------------
add_library(sing_dsp STATIC
    source/dsp/Engine.cpp
    source/dsp/VoiceData.cpp
    source/dsp/VoiceSynth.cpp)
target_include_directories(sing_dsp PUBLIC source)
# The library is linked into plugin bundles, which are shared libraries.
set_target_properties(sing_dsp PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(sing_dsp PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(sing_dsp PRIVATE /W4)
else()
    target_compile_options(sing_dsp PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
endif()

# ---------------------------------------------------------------------------
# Plugin
# ---------------------------------------------------------------------------
if(SING_BUILD_PLUGIN)
    set(SING_JUCE_DIR "" CACHE PATH "Optional local JUCE checkout (otherwise downloaded)")
    if(SING_JUCE_DIR)
        add_subdirectory(${SING_JUCE_DIR} juce EXCLUDE_FROM_ALL)
    else()
        FetchContent_Declare(JUCE
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG 8.0.10
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(JUCE)
    endif()

    juce_add_plugin(Sing
        PRODUCT_NAME "Sing"
        COMPANY_NAME "Overcast Audio"
        BUNDLE_ID "com.overcastaudio.sing"
        PLUGIN_MANUFACTURER_CODE Ovca
        PLUGIN_CODE Sng1
        FORMATS VST3 AU Standalone
        IS_SYNTH TRUE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        AU_MAIN_TYPE kAudioUnitType_MusicDevice
        VST3_CATEGORIES Instrument Sampler
        COPY_PLUGIN_AFTER_BUILD ${APPLE})

    # Fonts for the panel (SIL Open Font License, see resources/fonts).
    juce_add_binary_data(SingAssets
        HEADER_NAME SingAssets.h
        NAMESPACE SingAssets
        SOURCES
            resources/fonts/CormorantGaramond-Medium.ttf
            resources/fonts/CormorantGaramond-SemiBold.ttf
            resources/fonts/Jost-Regular.ttf
            resources/fonts/Jost-Medium.ttf)
    set_target_properties(SingAssets PROPERTIES POSITION_INDEPENDENT_CODE ON)

    # Panel sources.
    set(SING_UI_SOURCES
        source/PluginEditor.cpp
        source/ui/Controls.cpp
        source/ui/Theme.cpp
        source/ui/BlueHourSky.cpp
        source/ui/VoiceHorizon.cpp)
    if(APPLE)
        list(APPEND SING_UI_SOURCES source/ui/ReducedMotion.mm)
    else()
        list(APPEND SING_UI_SOURCES source/ui/ReducedMotion.cpp)
    endif()

    set(SING_PLUGIN_SOURCES
        source/PluginProcessor.cpp
        source/Parameters.cpp
        source/PresetManager.cpp
        source/VoiceLibrary.cpp)

    target_sources(Sing PRIVATE ${SING_PLUGIN_SOURCES} ${SING_UI_SOURCES})
    target_include_directories(Sing PRIVATE source)

    target_compile_definitions(Sing PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)

    target_link_libraries(Sing
        PRIVATE
            sing_dsp
            SingAssets
            juce::juce_audio_utils
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)

    if(SING_BUILD_SNAPSHOT)
        juce_add_console_app(SingSnapshot PRODUCT_NAME "SingSnapshot")
        target_sources(SingSnapshot PRIVATE tools/Snapshot.cpp ${SING_PLUGIN_SOURCES} ${SING_UI_SOURCES})
        target_include_directories(SingSnapshot PRIVATE source)
        target_compile_definitions(SingSnapshot PRIVATE
            JucePlugin_Name="Sing"
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0)
        target_link_libraries(SingSnapshot PRIVATE
            sing_dsp
            SingAssets
            juce::juce_audio_utils
            juce::juce_dsp
            juce::juce_recommended_config_flags)
    endif()
endif()

# ---------------------------------------------------------------------------
# Development tools
# ---------------------------------------------------------------------------
if(SING_BUILD_TOOLS)
    add_executable(sing_render tools/Render.cpp)
    target_link_libraries(sing_render PRIVATE sing_dsp)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(SING_BUILD_TESTS)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.7.1
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(Catch2)

    add_executable(sing_tests
        tests/PitchTests.cpp
        tests/VoiceTests.cpp
        tests/EngineTests.cpp)
    target_link_libraries(sing_tests PRIVATE sing_dsp Catch2::Catch2WithMain)

    enable_testing()
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)
    catch_discover_tests(sing_tests)
endif()

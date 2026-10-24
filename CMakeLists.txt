cmake_minimum_required (VERSION 3.22)

# Universal binary and minimum macOS version must be set before project().
if (APPLE)
    set (CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
    if (NOT CMAKE_OSX_ARCHITECTURES)
        set (CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build architectures")
    endif()
endif()

project (Sing VERSION 0.1.0 LANGUAGES C CXX)

set (CMAKE_CXX_STANDARD 20)
set (CMAKE_CXX_STANDARD_REQUIRED ON)
set (CMAKE_CXX_EXTENSIONS OFF)

option (SING_BUILD_PLUGIN "Build the JUCE plugin (VST3, AU, Standalone)" ON)
option (SING_BUILD_TESTS "Build the DSP tests" ON)
option (SING_BUILD_TOOLS "Build the offline render tool" ON)
option (SING_BUILD_SNAPSHOT "Build a tool that renders the panel to PNG (development only)" OFF)

include (FetchContent)

# ---------------------------------------------------------------------------
# DSP library: plain C++, no JUCE, so it builds and is tested on any platform.
# ---------------------------------------------------------------------------
add_library (SingDSP STATIC
    Source/DSP/Stretch/LoopSource.cpp
    Source/DSP/Common/RealFFT.cpp
    Source/DSP/Stretch/StretchEngine.cpp
    Source/DSP/Stretch/GranularEngine.cpp
    Source/DSP/Voice/FormantShifter.cpp
    Source/DSP/Voice/PlaceholderVoices.cpp
    Source/DSP/Voice/SingSynth.cpp
    Source/DSP/Voice/SingVoice.cpp
    Source/DSP/Voice/SourceExchange.cpp
    Source/DSP/Voice/VoiceAnalysis.cpp)
target_include_directories (SingDSP PUBLIC Source/DSP)
# The library is linked into plugin bundles, which are shared libraries.
set_target_properties (SingDSP PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features (SingDSP PUBLIC cxx_std_20)
if (MSVC)
    target_compile_options (SingDSP PRIVATE /W4)
else()
    target_compile_options (SingDSP PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
endif()

if (SING_BUILD_TOOLS)
    add_executable (sing-stretch-render Tools/stretch-render/main.cpp)
    target_link_libraries (sing-stretch-render PRIVATE SingDSP)
endif()

# ---------------------------------------------------------------------------
# Plugin
# ---------------------------------------------------------------------------
if (SING_BUILD_PLUGIN)
    set (SING_JUCE_DIR "" CACHE PATH "Optional local JUCE checkout (otherwise downloaded)")
    if (SING_JUCE_DIR)
        add_subdirectory (${SING_JUCE_DIR} juce EXCLUDE_FROM_ALL)
    else()
        FetchContent_Declare (JUCE
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG 8.0.10
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable (JUCE)
    endif()

    juce_add_plugin (Sing
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

    # Fonts for the panel (SIL Open Font License, see Resources/Fonts).
    juce_add_binary_data (SingAssets
        HEADER_NAME SingAssets.h
        NAMESPACE SingAssets
        SOURCES
            Resources/Fonts/CormorantGaramond-Medium.ttf
            Resources/Fonts/CormorantGaramond-SemiBold.ttf
            Resources/Fonts/Jost-Regular.ttf
            Resources/Fonts/Jost-Medium.ttf)
    set_target_properties (SingAssets PROPERTIES POSITION_INDEPENDENT_CODE ON)

    set (SING_PLUGIN_SOURCES
        Source/Plugin/PluginProcessor.cpp
        Source/Plugin/Parameters.cpp
        Source/Plugin/PresetManager.cpp
        Source/Plugin/VoiceData.cpp)
    set (SING_UI_SOURCES
        Source/Plugin/PluginEditor.cpp
        Source/Plugin/UI/BlueHourSky.cpp
        Source/Plugin/UI/Controls.cpp
        Source/Plugin/UI/Theme.cpp
        Source/Plugin/UI/WaveformView.cpp)
    if (APPLE)
        list (APPEND SING_UI_SOURCES Source/Plugin/UI/ReducedMotion.mm)
    else()
        list (APPEND SING_UI_SOURCES Source/Plugin/UI/ReducedMotion.cpp)
    endif()

    target_sources (Sing PRIVATE ${SING_PLUGIN_SOURCES} ${SING_UI_SOURCES})
    target_include_directories (Sing PRIVATE Source/Plugin)

    set (SING_JUCE_DEFINITIONS
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)
    target_compile_definitions (Sing PUBLIC ${SING_JUCE_DEFINITIONS})

    target_link_libraries (Sing
        PRIVATE
            SingDSP
            SingAssets
            juce::juce_audio_utils
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)

    if (SING_BUILD_SNAPSHOT)
        juce_add_console_app (SingSnapshot PRODUCT_NAME "SingSnapshot")
        target_sources (SingSnapshot PRIVATE Tools/Snapshot.cpp ${SING_PLUGIN_SOURCES} ${SING_UI_SOURCES})
        target_include_directories (SingSnapshot PRIVATE Source/Plugin)
        target_compile_definitions (SingSnapshot PRIVATE JucePlugin_Name="Sing" ${SING_JUCE_DEFINITIONS})
        target_link_libraries (SingSnapshot PRIVATE
            SingDSP
            SingAssets
            juce::juce_audio_utils
            juce::juce_dsp
            juce::juce_recommended_config_flags)
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if (SING_BUILD_TESTS)
    FetchContent_Declare (Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.7.1
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable (Catch2)

    add_executable (SingDSPTests
        Tests/StretchTests.cpp
        Tests/VoiceTests.cpp)
    target_link_libraries (SingDSPTests PRIVATE SingDSP Catch2::Catch2WithMain)

    enable_testing()
    list (APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include (Catch)
    catch_discover_tests (SingDSPTests)
endif()

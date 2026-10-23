cmake_minimum_required (VERSION 3.22)

project (Sing VERSION 0.1.0 LANGUAGES CXX)

set (CMAKE_CXX_STANDARD 17)
set (CMAKE_CXX_STANDARD_REQUIRED ON)

# Sing's DSP, in plain C++17 with no JUCE dependency, so it builds and tests anywhere.
# The JUCE plugin target links against it.
add_library (SingDSP STATIC
    Source/DSP/Stretch/LoopSource.cpp
    Source/DSP/Stretch/StretchEngine.cpp
    Source/DSP/Stretch/GranularEngine.cpp)

target_include_directories (SingDSP PUBLIC Source/DSP)

if (MSVC)
    target_compile_options (SingDSP PRIVATE /W4)
else()
    target_compile_options (SingDSP PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wno-sign-conversion)
endif()

option (SING_BUILD_TOOLS "Build the offline render tool" ON)
option (SING_BUILD_TESTS "Build the DSP tests" ON)

if (SING_BUILD_TOOLS)
    add_executable (sing-stretch-render Tools/stretch-render/main.cpp)
    target_link_libraries (sing-stretch-render PRIVATE SingDSP)
endif()

if (SING_BUILD_TESTS)
    enable_testing()
    add_executable (SingDSPTests Tests/StretchTests.cpp)
    target_link_libraries (SingDSPTests PRIVATE SingDSP)
    add_test (NAME SingDSPTests COMMAND SingDSPTests)
endif()

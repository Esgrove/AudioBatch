# ImageControl
# A multiband mid/side stereo imaging effect by Esgrove, as a VST3 and, on macOS, an Audio Unit,
# with a spatial spectrogram editor.
# Builds on its own with the JUCE submodule of the AudioBatch C++ project.

cmake_minimum_required(VERSION 4.2)

project(
    "ImageControl"
    VERSION 1.0.0
    DESCRIPTION "Multiband stereo imaging plugin with mid/side and pan controls"
    LANGUAGES CXX C
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum macOS version")
endif()

option(IMAGE_CONTROL_BUILD_TESTS "Build the ImageControl unit tests and benchmark" ${PROJECT_IS_TOP_LEVEL})
option(IMAGE_CONTROL_HARDENING "Enable standard library hardening, as the AudioBatch build does" ON)

# The same CPU targets and optimizations as AudioBatch:
# Release link-time optimization, fused multiply-adds, the local CPU in local Release builds but never in CI,
# and an AVX2 baseline on Windows. macOS builds for the host architecture, which is arm64 with NEON on Apple Silicon.
include("${CMAKE_CURRENT_SOURCE_DIR}/../cpp/cmake/Optimizations.cmake")

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../cpp/JUCE" JUCE)

# VST3 everywhere, and an Audio Unit on macOS from the same code.
set(IMAGE_CONTROL_FORMATS VST3)
if(APPLE)
    list(APPEND IMAGE_CONTROL_FORMATS AU)
endif()

# build.sh --install copies the bundles to the user's plugin folders, so the build itself never does.
juce_add_plugin(
    ImageControl
    COMPANY_NAME
    "Esgrove"
    BUNDLE_ID
    com.esgrove.imagecontrol
    PLUGIN_MANUFACTURER_CODE
    Esgr
    PLUGIN_CODE
    Egic
    FORMATS
    ${IMAGE_CONTROL_FORMATS}
    AU_MAIN_TYPE
    kAudioUnitType_Effect
    PRODUCT_NAME
    "ImageControl"
    VERSION
    ${PROJECT_VERSION}
    IS_SYNTH
    FALSE
    NEEDS_MIDI_INPUT
    FALSE
    NEEDS_MIDI_OUTPUT
    FALSE
    IS_MIDI_EFFECT
    FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS
    FALSE
    COPY_PLUGIN_AFTER_BUILD
    FALSE
    VST3_CATEGORIES
    Fx
    Spatial
    NEEDS_WEB_BROWSER
    FALSE
    NEEDS_CURL
    FALSE
)

set(IMAGE_CONTROL_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ImageControlEditor.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ImageControlEditor.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ImageControlProcessor.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ImageControlProcessor.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Parameters.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Parameters.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/analysis/SpatialAnalyzer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/analysis/SpatialAnalyzer.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/analysis/StereoSampleFifo.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/CrossoverDesign.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/CrossoverDesign.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/MultibandImager.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/MultibandImager.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/PartitionedConvolver.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/PartitionedConvolver.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/BandStrip.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/BandStrip.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/ControlSlider.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/ControlSlider.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/ImageControlLookAndFeel.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/ImageControlLookAndFeel.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/SettingsPanel.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/SettingsPanel.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/SpatialSpectrogramView.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/SpatialSpectrogramView.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/SpectrogramRenderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/SpectrogramRenderer.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/SplitDivider.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/SplitDivider.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/TooltipOverlay.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/TooltipOverlay.h"
)
target_sources(ImageControl PRIVATE ${IMAGE_CONTROL_SOURCES})
target_include_directories(ImageControl PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_definitions(
    ImageControl
    PUBLIC
        JUCE_DISABLE_JUCE_VERSION_PRINTING=1
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_WEB_BROWSER=0
        JUCE_WIN_PER_MONITOR_DPI_AWARE=1
)

# The spectrogram draws its particles with OpenGL through a context attached to the editor,
# which also paints the editor's components over the GL picture.
# On macOS, the editor also presents through JUCE's Metal layer renderer while no GL context is attached,
# with Core Graphics painting each changed region into a Metal texture instead of one merged region.
if(APPLE)
    target_compile_definitions(ImageControl PUBLIC JUCE_COREGRAPHICS_RENDER_WITH_MULTIPLE_PAINT_CALLS=1)
endif()

# Standard library hardening, as in the AudioBatch build:
# an out-of-range container access or an empty optional stops the host at the bug
# instead of silently corrupting its memory.
# Release builds use the fast mode, whose checks cost little, and Debug builds the extensive one.
# The benchmark measures its cost on the hot paths, built once with and once without IMAGE_CONTROL_HARDENING.
if(IMAGE_CONTROL_HARDENING)
    if(APPLE)
        target_compile_definitions(
            ImageControl
            PUBLIC
                _LIBCPP_HARDENING_MODE=$<IF:$<CONFIG:Debug>,_LIBCPP_HARDENING_MODE_EXTENSIVE,_LIBCPP_HARDENING_MODE_FAST>
        )
    elseif(MSVC)
        target_compile_definitions(ImageControl PUBLIC _MSVC_STL_HARDENING=1)
    endif()
endif()

# The plugin's own sources get strict warnings as errors, like the AudioBatch sources.
# JUCE headers are written to JUCE's own rules, so they are treated as system or external headers.
# The JUCE module sources compiled into the target keep JUCE's recommended warnings only.
set(IMAGE_CONTROL_JUCE_MODULES "${CMAKE_CURRENT_SOURCE_DIR}/../cpp/JUCE/modules")
if(MSVC)
    set(IMAGE_CONTROL_WARNINGS
        /W4
        /WX
        /permissive-
        /utf-8
        /external:I
        "${IMAGE_CONTROL_JUCE_MODULES}"
        /external:W0
    )
else()
    set(IMAGE_CONTROL_WARNINGS
        -isystem
        "${IMAGE_CONTROL_JUCE_MODULES}"
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wdouble-promotion
        -Wfloat-equal
        -Wold-style-cast
        -Wnon-virtual-dtor
        -Wimplicit-fallthrough
        -Werror
    )
endif()
set(IMAGE_CONTROL_COMPILED_SOURCES ${IMAGE_CONTROL_SOURCES})
list(FILTER IMAGE_CONTROL_COMPILED_SOURCES INCLUDE REGEX "\\.cpp$")
set_source_files_properties(
    ${IMAGE_CONTROL_COMPILED_SOURCES} TARGET_DIRECTORY ImageControl
    PROPERTIES COMPILE_OPTIONS "${IMAGE_CONTROL_WARNINGS}"
)

# The editor uses the same Roboto fonts as AudioBatch, from the shared assets.
juce_add_binary_data(
    ImageControlAssets
    HEADER_NAME ImageControlBinaryData.h
    NAMESPACE ImageControlBinaryData
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../assets/Roboto-Regular.ttf"
            "${CMAKE_CURRENT_SOURCE_DIR}/../assets/RobotoMono-Regular.ttf"
)

target_link_libraries(
    ImageControl
    PRIVATE juce::juce_audio_processors juce::juce_audio_utils juce::juce_dsp juce::juce_opengl ImageControlAssets
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags
)

# The ImageControl_pluginval target validates the VST3, when configured with -DAUDIOBATCH_PLUGINVAL=ON.
include("${CMAKE_CURRENT_SOURCE_DIR}/../cpp/cmake/Pluginval.cmake")
audiobatch_add_pluginval(ImageControl)

if(IMAGE_CONTROL_BUILD_TESTS)
    include(CTest)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        # v1.18.0
        GIT_TAG 063de7e9578f82b369302001269680b4b1553359
        SYSTEM
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    set(IMAGE_CONTROL_TEST_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/AnalyzerTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ConvolverTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/CrossoverTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ImagerTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ProcessorTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/SpectrogramViewTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/TestMain.cpp"
    )
    add_executable(ImageControlTests ${IMAGE_CONTROL_TEST_SOURCES})
    target_link_libraries(ImageControlTests PRIVATE ImageControl GTest::gtest)
    target_include_directories(ImageControlTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    set_source_files_properties(
        ${IMAGE_CONTROL_TEST_SOURCES} TARGET_DIRECTORY ImageControlTests
        PROPERTIES COMPILE_OPTIONS "${IMAGE_CONTROL_WARNINGS}"
    )
    include(GoogleTest)
    gtest_discover_tests(ImageControlTests DISCOVERY_TIMEOUT 60)

    # Times the audio processing and the spectrogram rendering. It is not a test, so CTest does not run it.
    add_executable(ImageControlBenchmark "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/Benchmark.cpp")
    target_link_libraries(ImageControlBenchmark PRIVATE ImageControl)
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/Benchmark.cpp" TARGET_DIRECTORY ImageControlBenchmark
        PROPERTIES COMPILE_OPTIONS "${IMAGE_CONTROL_WARNINGS}"
    )

    # Writes AudioBatch chain presets for the installed VST3 and Audio Unit,
    # which benchmarks/compare_formats.py renders and times through the AudioBatch CLI.
    juce_add_console_app(ImageControlPresetWriter PRODUCT_NAME "ImageControlPresetWriter")
    target_sources(
        ImageControlPresetWriter PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/PresetWriter.cpp"
                                         "${CMAKE_CURRENT_SOURCE_DIR}/src/Parameters.cpp"
    )
    target_include_directories(ImageControlPresetWriter PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(
        ImageControlPresetWriter PRIVATE JUCE_PLUGINHOST_AU=1 JUCE_PLUGINHOST_VST3=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0
    )
    target_link_libraries(
        ImageControlPresetWriter
        PRIVATE juce::juce_audio_processors juce::juce_audio_utils
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags
    )
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/PresetWriter.cpp" TARGET_DIRECTORY ImageControlPresetWriter
        PROPERTIES COMPILE_OPTIONS "${IMAGE_CONTROL_WARNINGS}"
    )
endif()

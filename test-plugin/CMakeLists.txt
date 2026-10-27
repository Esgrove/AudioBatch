# AudioBatch Test MS
# A minimal JUCE VST3 effect hosted by the C++ and the Rust test suites.
# Builds on its own with the JUCE submodule of the C++ project, or as a subdirectory of the C++ project.

cmake_minimum_required(VERSION 4.2)

project(
    "AudioBatchTestPlugin"
    VERSION 1.0.0
    DESCRIPTION "Mid/side and gain test plugin for the AudioBatch tests"
    LANGUAGES CXX C
)

if(NOT COMMAND juce_add_plugin)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    if(APPLE)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum macOS version")
    endif()
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../cpp/JUCE" JUCE)
endif()

# The plugin is never copied to a system plugin folder, so it only shows up where a test points to it.
# Legacy parameter ids make the VST3 parameter ids the parameter indices, which the tests rely on.
juce_add_plugin(
    AudioBatchTestPlugin
    COMPANY_NAME
    "AudioBatch Tests"
    BUNDLE_ID
    com.esgrove.audiobatch.testplugin
    PLUGIN_MANUFACTURER_CODE
    Abts
    PLUGIN_CODE
    Abms
    FORMATS
    VST3
    PRODUCT_NAME
    "AudioBatch Test MS"
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
    NEEDS_WEB_BROWSER
    FALSE
    NEEDS_CURL
    FALSE
)

target_sources(
    AudioBatchTestPlugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/MidSideTestProcessor.cpp"
                                 "${CMAKE_CURRENT_SOURCE_DIR}/src/MidSideTestProcessor.h"
)

target_compile_definitions(
    AudioBatchTestPlugin PUBLIC JUCE_DISABLE_JUCE_VERSION_PRINTING=1 JUCE_FORCE_USE_LEGACY_PARAM_IDS=1 JUCE_USE_CURL=0
                                JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_WEB_BROWSER=0
)

# The expected outputs in the fixtures are computed with separate float multiplies and adds,
# so the compiler must not fuse them into fused multiply-adds.
if(NOT MSVC)
    target_compile_options(AudioBatchTestPlugin PRIVATE -ffp-contract=off)
endif()

target_link_libraries(
    AudioBatchTestPlugin
    PRIVATE juce::juce_audio_processors
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags
)

# Path of the built VST3 bundle, for the C++ tests that load it.
# JUCE stores the path as a generator expression itself, so it is evaluated once more for the plugin target.
set(AUDIOBATCH_TEST_PLUGIN_BUNDLE
    "$<TARGET_GENEX_EVAL:AudioBatchTestPlugin_VST3,$<TARGET_PROPERTY:AudioBatchTestPlugin_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>"
)
if(NOT PROJECT_IS_TOP_LEVEL)
    set(AUDIOBATCH_TEST_PLUGIN_BUNDLE "${AUDIOBATCH_TEST_PLUGIN_BUNDLE}" PARENT_SCOPE)
endif()

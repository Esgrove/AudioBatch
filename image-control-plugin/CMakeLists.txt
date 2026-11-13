# ImageControl
# A multiband mid/side stereo imaging effect, as a VST3 and, on macOS, an Audio Unit,
# with a spatial spectrogram editor.
# Builds on its own with the JUCE submodule of the AudioBatch C++ project.

cmake_minimum_required(VERSION 4.2)

if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum macOS version")
endif()

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

option(IMAGE_CONTROL_BUILD_TESTS "Build the ImageControl unit tests and benchmark" ${PROJECT_IS_TOP_LEVEL})
option(IMAGE_CONTROL_HARDENING "Enable standard library hardening, as the AudioBatch build does" ON)

# Plugin-only sanitizer and coverage flags must be set before JUCE or any other targets are added.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Verification.cmake")

# The same CPU targets and optimizations as AudioBatch:
# Release link-time optimization, fused multiply-adds, the local CPU in local Release builds but never in CI,
# and an AVX2 baseline on Windows. macOS builds for the host architecture, which is arm64 with NEON on Apple Silicon.
include("${CMAKE_CURRENT_SOURCE_DIR}/../cpp/cmake/Optimizations.cmake")

# Visible-editor benchmark telemetry records the plugin's effective build, not the host executable's profile.
add_compile_definitions(
    "IMAGE_CONTROL_BUILD_RELEASE=$<IF:$<CONFIG:Release>,1,0>"
    "IMAGE_CONTROL_BUILD_NATIVE_CPU=$<IF:$<AND:$<CONFIG:Release>,$<BOOL:${use_native}>>,1,0>"
)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../cpp/JUCE" JUCE)

# VST3 everywhere, and an Audio Unit on macOS from the same code.
set(IMAGE_CONTROL_DEFAULT_FORMATS VST3)
if(APPLE)
    list(APPEND IMAGE_CONTROL_DEFAULT_FORMATS AU)
endif()
set(IMAGE_CONTROL_FORMATS "${IMAGE_CONTROL_DEFAULT_FORMATS}" CACHE STRING "Plugin formats to build (VST3;AU)")

# install.sh copies the bundles to the user's plugin folders, so the build itself never does.
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

# Ninja's linker signature seals only the plugin executable, not its completed bundle or generated moduleinfo.json.
# Sign both bundles with Hardened Runtime after resource and moduleinfo generation.
if(APPLE)
    if(TARGET ImageControl_VST3)
        set_target_properties(
            ImageControl_VST3 PROPERTIES XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED
                                                                                 NO
        )
        add_custom_target(
            ImageControl_VST3Sign ALL
            COMMAND /bin/bash "${CMAKE_CURRENT_SOURCE_DIR}/../common.sh" sign_mac_plugin_bundle
                    "$<TARGET_BUNDLE_DIR:ImageControl_VST3>"
            DEPENDS ImageControl_VST3
            VERBATIM
        )
    endif()
    if(TARGET ImageControl_AU)
        set_target_properties(
            ImageControl_AU PROPERTIES XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED NO
        )
        add_custom_target(
            ImageControl_AUSign ALL
            COMMAND /bin/bash "${CMAKE_CURRENT_SOURCE_DIR}/../common.sh" sign_mac_plugin_bundle
                    "$<TARGET_BUNDLE_DIR:ImageControl_AU>"
            DEPENDS ImageControl_AU
            VERBATIM
        )
    endif()
endif()

set(IMAGE_CONTROL_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ImageControlEditor.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ImageControlEditorControls.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ImageControlEditorWindows.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ImageControlEditor.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ImageControlProcessor.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ImageControlProcessor.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Parameters.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Parameters.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ParameterHistory.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ParameterHistory.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ProcessingState.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ProcessingState.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/UserPresets.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/UserPresets.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/analysis/SpatialAnalyzer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/analysis/SpatialAnalyzer.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/analysis/StereoSampleFifo.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/BandCorrelation.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/BandCorrelation.h"
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
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/DisplayDisclosure.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/DisplayDisclosure.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/HistoryAttachments.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/HistoryAttachments.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/ProcessingToolbar.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/ProcessingToolbar.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/ProcessingToolbarContent.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/ProcessingToolbarPresets.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/ImageControlLookAndFeel.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/ImageControlLookAndFeel.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/SettingsPanel.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/SettingsPanel.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/KeyboardShortcutsPanel.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/KeyboardShortcutsPanel.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/SpatialSpectrogramView.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/SpatialSpectrogramViewInput.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/SpatialSpectrogramView.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/SpectrogramParticles.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/SpectrogramParticles.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/SpectrogramRenderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/SpectrogramRenderer.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/SpectrogramWorker.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/SpectrogramWorker.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/RenderBenchmark.h"
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

# The editor embeds its GLSL sources and the same Roboto fonts as AudioBatch, from the shared assets.
juce_add_binary_data(
    ImageControlAssets
    HEADER_NAME ImageControlBinaryData.h
    NAMESPACE ImageControlBinaryData
    SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/../assets/Roboto-Regular.ttf"
        "${CMAKE_CURRENT_SOURCE_DIR}/../assets/Roboto-SemiBold.ttf"
        "${CMAKE_CURRENT_SOURCE_DIR}/../assets/RobotoMono-Regular.ttf"
        "${CMAKE_CURRENT_SOURCE_DIR}/../assets/RobotoMono-SemiBold.ttf"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/shaders/screen.vert"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/shaders/fade.frag"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/shaders/particle.vert"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/shaders/particle.frag"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/shaders/downsample.frag"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/shaders/upsample.frag"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/shaders/composite.frag"
)

target_link_libraries(
    ImageControl
    PRIVATE
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_opengl
        ImageControlAssets
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags
)

# The ImageControl_pluginval target validates the VST3, when configured with -DAUDIOBATCH_PLUGINVAL=ON.
include("${CMAKE_CURRENT_SOURCE_DIR}/../cpp/cmake/Pluginval.cmake")
audiobatch_add_pluginval(ImageControl)

if(IMAGE_CONTROL_BUILD_TESTS)
    include(CTest)
    # IMAGE_CONTROL_BUILD_TESTS owns this project's tests, even in a cache with BUILD_TESTING=OFF.
    enable_testing()

    # {fmt} prints the console output of the benchmark, the preset writer, and the RTSan host.
    # The plugin and its format wrappers never link it, since plugin code does not print.
    # The pin matches the AudioBatch application in ../cpp/CMakeLists.txt.
    # SYSTEM exempts its headers from the warnings-as-errors flags.
    include(FetchContent)
    FetchContent_Declare(
        fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        # 12.2.1
        GIT_TAG 208219daff58f2a7438f6931cc2ddc358acf33ef
        SYSTEM
    )
    set(FMT_DOC OFF CACHE BOOL "" FORCE)
    set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
    set(FMT_TEST OFF CACHE BOOL "" FORCE)
    set(FMT_FUZZ OFF CACHE BOOL "" FORCE)
    set(FMT_MODULE OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(fmt)

    # The independent parity host compiles its own JUCE modules. Production plugin targets never host plugins.
    get_filename_component(IMAGE_CONTROL_PARITY_REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)
    juce_add_console_app(ImageControlParity PRODUCT_NAME "ImageControlParity")
    target_sources(
        ImageControlParity
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/PluginParity.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/PluginParityScenarios.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/PluginParityScenarios.h"
    )
    target_include_directories(ImageControlParity PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(
        ImageControlParity
        PRIVATE
            JUCE_PLUGINHOST_VST3=1
            JUCE_USE_CURL=0
            JUCE_WEB_BROWSER=0
            JUCE_DISABLE_JUCE_VERSION_PRINTING=1
            IMAGE_CONTROL_PARITY_REPOSITORY="${IMAGE_CONTROL_PARITY_REPOSITORY}"
    )
    target_link_libraries(
        ImageControlParity
        PRIVATE juce::juce_audio_processors juce::juce_audio_utils fmt::fmt
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags
    )
    if(IMAGE_CONTROL_HARDENING)
        if(APPLE)
            target_compile_definitions(
                ImageControlParity
                PRIVATE
                    _LIBCPP_HARDENING_MODE=$<IF:$<CONFIG:Debug>,_LIBCPP_HARDENING_MODE_EXTENSIVE,_LIBCPP_HARDENING_MODE_FAST>
            )
        elseif(MSVC)
            target_compile_definitions(ImageControlParity PRIVATE _MSVC_STL_HARDENING=1)
        endif()
    endif()
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/PluginParity.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/PluginParityScenarios.cpp" TARGET_DIRECTORY ImageControlParity
        PROPERTIES COMPILE_OPTIONS "${IMAGE_CONTROL_WARNINGS}"
    )
    set(IMAGE_CONTROL_PARITY_RUST_BUNDLE "" CACHE PATH
                                                  "Explicit Rust build-tree VST3 for CTest cross-implementation parity"
    )
    if(IMAGE_CONTROL_PARITY_RUST_BUNDLE)
        add_test(
            NAME ImageControlParity.Plugins
            COMMAND ImageControlParity --cpp "$<TARGET_BUNDLE_DIR:ImageControl_VST3>" --rust
                    "${IMAGE_CONTROL_PARITY_RUST_BUNDLE}" --report "${CMAKE_CURRENT_BINARY_DIR}/plugin-parity.json"
        )
        set_tests_properties(ImageControlParity.Plugins PROPERTIES LABELS "parity;integration" TIMEOUT 300)
    endif()

    if(IMAGE_CONTROL_SANITIZE STREQUAL "realtime")
        # This process hosts the instrumented build-tree bundle with the same startup-linked RTSan runtime.
        add_executable(ImageControlRealtimeTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/RealtimeTests.cpp")
        target_link_libraries(ImageControlRealtimeTests PRIVATE ImageControl fmt::fmt)
        # Compile the host implementation into shared JUCE code only in this isolated verification tree.
        target_compile_definitions(ImageControl PRIVATE JUCE_PLUGINHOST_VST3=1)
        target_compile_definitions(
            ImageControlRealtimeTests PRIVATE JUCE_PLUGINHOST_VST3=1
                                              IMAGE_CONTROL_REALTIME_VST3_PATH="$<TARGET_BUNDLE_DIR:ImageControl_VST3>"
        )
        target_link_options(ImageControlRealtimeTests PRIVATE -fsanitize=realtime)
        if(APPLE AND TARGET ImageControl_VST3Sign)
            add_dependencies(ImageControlRealtimeTests ImageControl_VST3Sign)
        else()
            add_dependencies(ImageControlRealtimeTests ImageControl_VST3)
        endif()
        set_source_files_properties(
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/RealtimeTests.cpp" TARGET_DIRECTORY ImageControlRealtimeTests
            PROPERTIES COMPILE_OPTIONS "${IMAGE_CONTROL_WARNINGS}"
        )
        find_program(IMAGE_CONTROL_UV_EXECUTABLE uv REQUIRED)

        # Each callback scenario gets its own process, report directory, and bounded shared runner.
        function(image_control_add_realtime_case case_name)
            set(case_arguments ${ARGN})
            # Successful and continuing scenarios must reach their own completion marker, not just exit quietly.
            # Fail-fast detector controls and blocked audits terminate before that marker by design.
            if(NOT "--expect" IN_LIST case_arguments OR "--diagnostic" IN_LIST case_arguments)
                list(APPEND case_arguments --require-output "^ImageControl RTSan scenario completed: ${case_name}$")
            endif()
            add_test(
                NAME ImageControlRealtime.${case_name}
                COMMAND
                    "${IMAGE_CONTROL_UV_EXECUTABLE}" run --script
                    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/realtime_sanitizer.py" --executable
                    "$<TARGET_FILE:ImageControlRealtimeTests>" --case "${case_name}" --artifacts
                    "${CMAKE_CURRENT_BINARY_DIR}/realtime-artifacts" ${case_arguments}
            )
            set_tests_properties(ImageControlRealtime.${case_name} PROPERTIES LABELS "realtime" TIMEOUT 45)
        endfunction()

        image_control_add_realtime_case(control-clean)
        image_control_add_realtime_case(
            control-allocate
            --control
            --expect
            malloc
            --stack
            allocateControl
        )
        image_control_add_realtime_case(
            control-free
            --control
            --expect
            free
            --stack
            freeControl
        )
        image_control_add_realtime_case(
            control-mutex
            --control
            --expect
            pthread_mutex_lock
            --stack
            mutexControl
        )
        set_tests_properties(
            ImageControlRealtime.control-clean ImageControlRealtime.control-allocate ImageControlRealtime.control-free
            ImageControlRealtime.control-mutex PROPERTIES LABELS "realtime;realtime-control"
        )
        foreach(
            case_name IN
            ITEMS
                direct-neutral-mono
                direct-neutral-stereo
                direct-active-mono
                direct-active-stereo
                direct-transitions
                imager-handoff-neutral
                imager-handoff-active
        )
            image_control_add_realtime_case(${case_name})
            set_tests_properties(ImageControlRealtime.${case_name} PROPERTIES LABELS "realtime;realtime-clean")
        endforeach()
        # Observed on arm64 macOS with Homebrew LLVM 23: the host locks its MIDI event list before entering the bundle.
        # These fail-fast audits are BLOCKED, not a claim that the hosted path is real-time safe.
        foreach(case_name IN ITEMS hosted-first-callback hosted-cached-parameters)
            image_control_add_realtime_case(
                ${case_name}
                "--argument=--case"
                "--argument=${case_name}"
                "--argument=--plugin"
                "--argument=$<TARGET_BUNDLE_DIR:ImageControl_VST3>"
                --expect
                pthread_mutex_lock
                --stack
                "juce::MidiEventList::clear"
                --stack
                "juce::VST3PluginInstanceHeadless::associateWith"
                --stack
                processHosted
            )
            set_tests_properties(
                ImageControlRealtime.${case_name} PROPERTIES LABELS "realtime;realtime-known-regression"
            )
        endforeach()

        # Continuing probes reach the bundle without suppressing the host's earlier findings.
        # The unchanged path has 8 reports: host clear/count lock-unlock pairs and the bundle callback lock-unlock.
        # A changed cached parameter adds one Darwin thread-local initialization calloc beneath parameter delivery.
        # Require the exact call/owner pair in each report, not an arbitrary mutex or allocator anywhere in the stack.
        set(IMAGE_CONTROL_HOSTED_LOCK_STACK
            "(?s:unsafe function .pthread_mutex_(lock|unlock). .*juce::(MidiEventList::(clear|getEventCount)|JuceVST3Component::processAudio))"
        )
        set(IMAGE_CONTROL_HOSTED_PARAMETER_STACK
            "(?s:unsafe function .calloc. .*juce::setValueAndNotifyIfChanged.*juce::JuceVST3Component::processParameterChanges)"
        )
        image_control_add_realtime_case(
            hosted-first-callback-diagnostic
            "--argument=--case"
            "--argument=hosted-first-callback-diagnostic"
            "--argument=--plugin"
            "--argument=$<TARGET_BUNDLE_DIR:ImageControl_VST3>"
            --diagnostic
            --expect
            pthread_mutex_lock
            --expect
            pthread_mutex_unlock
            --stack
            "${IMAGE_CONTROL_HOSTED_LOCK_STACK}"
            --stack
            processHosted
            --require-output
            "^[ ]*Unique error count: 8[ ]*$"
        )
        image_control_add_realtime_case(
            hosted-cached-parameters-diagnostic
            "--argument=--case"
            "--argument=hosted-cached-parameters-diagnostic"
            "--argument=--plugin"
            "--argument=$<TARGET_BUNDLE_DIR:ImageControl_VST3>"
            --diagnostic
            --expect
            pthread_mutex_lock
            --expect
            pthread_mutex_unlock
            --expect
            calloc
            --stack
            "${IMAGE_CONTROL_HOSTED_LOCK_STACK}|${IMAGE_CONTROL_HOSTED_PARAMETER_STACK}"
            --stack
            processHosted
            --require-output
            "^[ ]*Unique error count: 9[ ]*$"
        )
        set_tests_properties(
            ImageControlRealtime.hosted-first-callback-diagnostic
            ImageControlRealtime.hosted-cached-parameters-diagnostic
            PROPERTIES LABELS "realtime;realtime-known-regression;realtime-diagnostic"
        )
    endif()
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
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ProcessingStateTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ParameterHistoryTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ComparisonTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/UserPresetsTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/HistoryControlTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/EditingWorkflowTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/CompactEditingWorkflowTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/PresetWorkflowTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/PresetResultTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/PresetWorkflowTestSupport.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ControlLayoutTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ControlLayoutTestSupport.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/BandStripTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/KeyboardShortcutsTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/CompactBandGeometryTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/CompactBandInputTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/CompactBandNumericTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/CompactBandTestSupport.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/CompactLayoutTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ConvolverTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/CorrelationMeterTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/CorrelationTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/CrossoverTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/SpectrogramOverlayTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ShaderSourceTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ImageControlEditorTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/HostContractsTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ImagerTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ProcessorTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ProcessorStateTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ProcessorTestSupport.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/RecoveryTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/SpectrogramViewTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/SpectrogramParticlesTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ViewInteractionTestSupport.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/SpectrogramWorkerTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/StateThreadingTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/StereoSampleFifoTests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/TestMain.cpp"
    )
    add_executable(ImageControlTests ${IMAGE_CONTROL_TEST_SOURCES})
    target_link_libraries(ImageControlTests PRIVATE ImageControl ImageControlAssets GTest::gtest)
    target_include_directories(ImageControlTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(
        ImageControlTests PRIVATE IMAGE_CONTROL_SHADER_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/src/ui/shaders"
    )
    if(WIN32)
        # windows.h defines min and max macros that break std::min and std::max in the tests.
        target_compile_definitions(ImageControlTests PRIVATE NOMINMAX)
    endif()
    set_source_files_properties(
        ${IMAGE_CONTROL_TEST_SOURCES} TARGET_DIRECTORY ImageControlTests
        PROPERTIES COMPILE_OPTIONS "${IMAGE_CONTROL_WARNINGS}"
    )
    include(GoogleTest)
    gtest_discover_tests(
        ImageControlTests
        DISCOVERY_MODE
        PRE_TEST
        DISCOVERY_TIMEOUT
        60
        PROPERTIES
        TIMEOUT 180 LABELS unit
    )

    # Times the audio processing and the spectrogram rendering. It is not a test, so CTest does not run it.
    add_executable(
        ImageControlBenchmark
        "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/Benchmark.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/BenchmarkSupport.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/CallbackBenchmarks.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/ComponentBenchmarks.cpp"
    )
    target_link_libraries(ImageControlBenchmark PRIVATE ImageControl fmt::fmt)
    target_include_directories(ImageControlBenchmark PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/Benchmark.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/CallbackBenchmarks.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/ComponentBenchmarks.cpp"
        TARGET_DIRECTORY
        ImageControlBenchmark
        PROPERTIES COMPILE_OPTIONS "${IMAGE_CONTROL_WARNINGS}"
    )

    # Isolated renderer benchmark compiles the production passes with optional Release GPU queries.
    # Its native helper owns a real context, while explicit offscreen batches exclude swap and presentation pacing.
    juce_add_console_app(ImageControlRenderBenchmark PRODUCT_NAME "ImageControlRenderBenchmark")
    target_sources(
        ImageControlRenderBenchmark
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/RenderBenchmark.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/Parameters.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/SpectrogramRenderer.cpp"
    )
    target_include_directories(ImageControlRenderBenchmark PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(
        ImageControlRenderBenchmark
        PRIVATE
            IMAGE_CONTROL_RENDER_BENCHMARK=1
            JUCE_USE_CURL=0
            JUCE_WEB_BROWSER=0
            JUCE_DISABLE_JUCE_VERSION_PRINTING=1
            JUCE_WIN_PER_MONITOR_DPI_AWARE=1
    )
    target_link_libraries(
        ImageControlRenderBenchmark
        PRIVATE
            juce::juce_audio_processors
            juce::juce_gui_basics
            juce::juce_opengl
            ImageControlAssets
            fmt::fmt
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags
    )
    if(IMAGE_CONTROL_HARDENING)
        if(APPLE)
            target_compile_definitions(
                ImageControlRenderBenchmark
                PRIVATE
                    _LIBCPP_HARDENING_MODE=$<IF:$<CONFIG:Debug>,_LIBCPP_HARDENING_MODE_EXTENSIVE,_LIBCPP_HARDENING_MODE_FAST>
            )
        elseif(MSVC)
            target_compile_definitions(ImageControlRenderBenchmark PRIVATE _MSVC_STL_HARDENING=1)
        endif()
    endif()
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/RenderBenchmark.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Parameters.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/SpectrogramRenderer.cpp"
        TARGET_DIRECTORY
        ImageControlRenderBenchmark
        PROPERTIES COMPILE_OPTIONS "${IMAGE_CONTROL_WARNINGS}"
    )

    # Record the selected configuration and effective native policy, not the requested cache option alone.
    # Debug and CI builds still compile these tools for help-only smoke checks, never for measurements.
    foreach(benchmark_target IN ITEMS ImageControlBenchmark ImageControlRenderBenchmark)
        target_compile_definitions(
            ${benchmark_target}
            PRIVATE
                IMAGE_CONTROL_BENCHMARK_BUILD_TYPE="$<CONFIG>"
                IMAGE_CONTROL_BENCHMARK_OPTIMIZED_BUILD=$<CONFIG:Release>
                IMAGE_CONTROL_BENCHMARK_NATIVE_CPU=$<AND:$<CONFIG:Release>,$<BOOL:${use_native}>>
        )
    endforeach()

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
        PRIVATE juce::juce_audio_processors juce::juce_audio_utils fmt::fmt
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags
    )
    if(IMAGE_CONTROL_HARDENING)
        if(APPLE)
            target_compile_definitions(
                ImageControlPresetWriter
                PRIVATE
                    _LIBCPP_HARDENING_MODE=$<IF:$<CONFIG:Debug>,_LIBCPP_HARDENING_MODE_EXTENSIVE,_LIBCPP_HARDENING_MODE_FAST>
            )
        elseif(MSVC)
            target_compile_definitions(ImageControlPresetWriter PRIVATE _MSVC_STL_HARDENING=1)
        endif()
    endif()
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/PresetWriter.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/Parameters.cpp"
        TARGET_DIRECTORY ImageControlPresetWriter PROPERTIES COMPILE_OPTIONS "${IMAGE_CONTROL_WARNINGS}"
    )
endif()

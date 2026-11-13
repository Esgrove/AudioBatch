# Release IPO, native CPU targeting, and fused multiply-add contraction are shared by the applications, tests,
# and source-built dependencies.
# Windows always requires AVX2. CI never adds host-specific CPU flags.

include(CheckCompilerFlag)
include(CheckCXXSourceCompiles)
include(CheckIPOSupported)

# Checks both languages because the audio meter and the SQLite fallback are compiled as C.
function(audiobatch_check_cpu_flag flag result)
    string(MAKE_C_IDENTIFIER "${flag}" flag_identifier)
    check_compiler_flag(C "${flag}" AUDIOBATCH_C_${flag_identifier})
    check_compiler_flag(CXX "${flag}" AUDIOBATCH_CXX_${flag_identifier})
    if(AUDIOBATCH_C_${flag_identifier} AND AUDIOBATCH_CXX_${flag_identifier})
        set(${result} TRUE PARENT_SCOPE)
    else()
        set(${result} FALSE PARENT_SCOPE)
    endif()
endfunction()

set(native_default ON)
if(DEFINED ENV{CI} OR CMAKE_CROSSCOMPILING)
    set(native_default OFF)
endif()
option(AUDIOBATCH_NATIVE_CPU "Target the local CPU in Release builds, never in CI" ${native_default})
set(AUDIOBATCH_TARGET_CPU "" CACHE STRING
                                   "Explicit Release target CPU when AUDIOBATCH_NATIVE_CPU is OFF, such as apple-m1"
)
option(AUDIOBATCH_LTO "Enable Release link-time optimization when supported" ON)

set(has_release FALSE)
if("Release" IN_LIST CMAKE_CONFIGURATION_TYPES OR CMAKE_BUILD_TYPE STREQUAL "Release")
    set(has_release TRUE)
endif()

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE FALSE)
if(AUDIOBATCH_LTO
   AND has_release
   AND NOT AUDIOBATCH_SANITIZE
)
    check_ipo_supported(
        RESULT ipo_supported
        OUTPUT ipo_output
        LANGUAGES C CXX
    )
    if(ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        message(STATUS "AudioBatch Release LTO: enabled")
        if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
            # Preserve JUCE's workaround for the Xcode 15 LTO linker bug.
            add_link_options("$<$<CONFIG:Release>:-Wl,-weak_reference_mismatches,weak>")
        endif()
    else()
        message(WARNING "AudioBatch Release LTO is not supported: ${ipo_output}")
    endif()
elseif(AUDIOBATCH_SANITIZE)
    message(STATUS "AudioBatch Release LTO: disabled for sanitizers")
else()
    message(STATUS "AudioBatch Release LTO: disabled or no Release configuration")
endif()

set(use_native ${AUDIOBATCH_NATIVE_CPU})
if(DEFINED ENV{CI})
    set(use_native FALSE)
    message(STATUS "AudioBatch native CPU targeting: disabled in CI")
elseif(CMAKE_CROSSCOMPILING)
    set(use_native FALSE)
    message(STATUS "AudioBatch native CPU targeting: disabled for cross-compilation")
elseif(AUDIOBATCH_SANITIZE OR NOT has_release)
    set(use_native FALSE)
    message(STATUS "AudioBatch native CPU targeting: disabled for sanitizers or no Release configuration")
elseif(APPLE AND CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES architecture_count)
    if(NOT architecture_count EQUAL 1 OR NOT CMAKE_OSX_ARCHITECTURES STREQUAL CMAKE_HOST_SYSTEM_PROCESSOR)
        set(use_native FALSE)
        message(STATUS "AudioBatch native CPU targeting: disabled for non-host or universal macOS architectures")
    endif()
endif()

set(cpu_languages "$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>")
set(is_apple_arm64 FALSE)
if(APPLE)
    if(CMAKE_OSX_ARCHITECTURES)
        list(LENGTH CMAKE_OSX_ARCHITECTURES osx_architecture_count)
        if(osx_architecture_count EQUAL 1 AND CMAKE_OSX_ARCHITECTURES MATCHES "^(arm64|aarch64|ARM64)$")
            set(is_apple_arm64 TRUE)
        endif()
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
        set(is_apple_arm64 TRUE)
    endif()
endif()

if(is_apple_arm64)
    # Explicitly enable JUCE ARM NEON SIMD paths and Apple's Accelerate vDSP framework across every target.
    add_compile_definitions(JUCE_USE_ARM_NEON=1 JUCE_USE_VDSP_FRAMEWORK=1)
endif()

if(APPLE
   AND has_release
   AND NOT AUDIOBATCH_SANITIZE
)
    # Strip unreachable Mach-O code sections in Release Ninja and Makefile links, matching Xcode dead-stripping.
    add_link_options("$<$<CONFIG:Release>:-Wl,-dead_strip>")
endif()

if(MSVC)
    audiobatch_check_cpu_flag("/arch:AVX2" avx2_supported)
    if(NOT avx2_supported)
        message(FATAL_ERROR "AudioBatch Windows builds require a compiler supporting /arch:AVX2 for C and C++")
    endif()

    set(release_architecture AVX2)
    if(use_native AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        foreach(flag /clang:-march=native /clang:-mtune=native)
            audiobatch_check_cpu_flag("${flag}" native_flag_supported)
            if(NOT native_flag_supported)
                message(
                    FATAL_ERROR "AudioBatch native CPU flag ${flag} is not supported. Use AUDIOBATCH_NATIVE_CPU=OFF."
                )
            endif()
            add_compile_options("$<$<AND:${cpu_languages},$<CONFIG:Release>>:${flag}>")
        endforeach()
        message(STATUS "AudioBatch Release native CPU: clang-cl -march=native -mtune=native")
    elseif(use_native)
        set_property(
            DIRECTORY
            APPEND
            PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/DetectNativeCpu.cpp"
        )
        # Do not cache a host result or compile the probe with the ISA it is trying to detect.
        try_run(
            cpu_run_result
            cpu_compile_result
            SOURCES
            "${CMAKE_CURRENT_LIST_DIR}/DetectNativeCpu.cpp"
            NO_CACHE
            COMPILE_DEFINITIONS /arch:SSE2
            COMPILE_OUTPUT_VARIABLE cpu_compile_output
            RUN_OUTPUT_VARIABLE cpu_output
        )
        if(NOT cpu_compile_result)
            message(FATAL_ERROR "AudioBatch native CPU probe failed to compile:\n${cpu_compile_output}")
        endif()
        if(NOT cpu_run_result STREQUAL "0")
            message(FATAL_ERROR "AudioBatch native CPU probe failed (${cpu_run_result}):\n${cpu_output}")
        endif()
        string(REPLACE "\r" "" cpu_output "${cpu_output}")
        if(NOT cpu_output MATCHES "^([A-Z0-9.;]+)\n(blend|INTEL64|AMD64)\n$")
            message(FATAL_ERROR "Unexpected AudioBatch native CPU probe output:\n${cpu_output}")
        endif()
        set(cpu_architectures "${CMAKE_MATCH_1}")
        set(cpu_tuning "${CMAKE_MATCH_2}")
        foreach(architecture IN LISTS cpu_architectures)
            audiobatch_check_cpu_flag("/arch:${architecture}" architecture_supported)
            if(architecture_supported)
                set(release_architecture "${architecture}")
                break()
            endif()
        endforeach()
        audiobatch_check_cpu_flag("/favor:${cpu_tuning}" tuning_supported)
        if(NOT tuning_supported)
            message(FATAL_ERROR "AudioBatch native CPU tuning /favor:${cpu_tuning} is not supported")
        endif()
        add_compile_options("$<$<AND:${cpu_languages},$<CONFIG:Release>>:/favor:${cpu_tuning}>")
        message(STATUS "AudioBatch Release native CPU: /arch:${release_architecture} /favor:${cpu_tuning}")
    endif()

    add_compile_options("$<${cpu_languages}:$<IF:$<CONFIG:Release>,/arch:${release_architecture},/arch:AVX2>>")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        add_compile_options("$<${cpu_languages}:/clang:-ffp-contract=fast>")
    else()
        add_compile_options("$<$<AND:${cpu_languages},$<CONFIG:Release>>:/fp:precise;/fp:contract>")
    endif()
    message(STATUS "AudioBatch Windows CPU baseline: AVX2")
elseif(use_native)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
        set(native_flags -mcpu=native -mtune=native)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i[3-6]86)$")
        set(native_flags -march=native -mtune=native)
    else()
        message(FATAL_ERROR "AudioBatch native CPU targeting is not supported for ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    foreach(flag IN LISTS native_flags)
        audiobatch_check_cpu_flag("${flag}" native_flag_supported)
        if(NOT native_flag_supported)
            message(FATAL_ERROR "AudioBatch native CPU flag ${flag} is not supported. Use AUDIOBATCH_NATIVE_CPU=OFF.")
        endif()
        add_compile_options("$<$<AND:${cpu_languages},$<CONFIG:Release>>:${flag}>")
        add_link_options("$<$<CONFIG:Release>:${flag}>")
    endforeach()
    message(STATUS "AudioBatch Release native CPU: ${native_flags}")
elseif(has_release AND NOT AUDIOBATCH_SANITIZE)
    set(explicit_target_cpu "${AUDIOBATCH_TARGET_CPU}")
    if(explicit_target_cpu STREQUAL "" AND is_apple_arm64)
        # Apple Silicon M1 (ARMv8.5-A) is the universal Apple Silicon baseline, providing NEON, FMA, FP16, and LSE.
        set(explicit_target_cpu "apple-m1")
    endif()
    if(NOT explicit_target_cpu STREQUAL "")
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$" OR is_apple_arm64)
            set(target_cpu_flags "-mcpu=${explicit_target_cpu}" "-mtune=${explicit_target_cpu}")
        else()
            set(target_cpu_flags "-march=${explicit_target_cpu}" "-mtune=${explicit_target_cpu}")
        endif()
        foreach(flag IN LISTS target_cpu_flags)
            audiobatch_check_cpu_flag("${flag}" target_cpu_flag_supported)
            if(NOT target_cpu_flag_supported)
                message(FATAL_ERROR "AudioBatch target CPU flag ${flag} is not supported by this compiler.")
            endif()
            add_compile_options("$<$<AND:${cpu_languages},$<CONFIG:Release>>:${flag}>")
            add_link_options("$<$<CONFIG:Release>:${flag}>")
        endforeach()
        message(STATUS "AudioBatch Release target CPU: ${target_cpu_flags}")
    endif()
endif()

if(is_apple_arm64)
    check_cxx_source_compiles(
        "#if !defined(__ARM_NEON) || !defined(__ARM_FEATURE_FMA)\n#error Apple Silicon builds require ARM NEON and hardware FMA\n#endif\nint main() { return 0; }"
        AUDIOBATCH_APPLE_ARM64_NEON_FMA
    )
    if(NOT AUDIOBATCH_APPLE_ARM64_NEON_FMA)
        message(FATAL_ERROR "Apple Silicon arm64 builds require __ARM_NEON and __ARM_FEATURE_FMA")
    endif()
endif()

if(NOT AUDIOBATCH_NATIVE_CPU)
    message(STATUS "AudioBatch native CPU targeting: disabled by AUDIOBATCH_NATIVE_CPU")
endif()

if(NOT MSVC)
    # Lets the compiler fuse multiply/add chains into FMA instructions across statements wherever the target has them.
    add_compile_options("$<$<AND:${cpu_languages},$<CONFIG:Release>>:-ffp-contract=fast>")
endif()

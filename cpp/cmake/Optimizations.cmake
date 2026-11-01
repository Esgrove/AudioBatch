# Release IPO and native CPU targeting are shared by the applications, tests, and source-built dependencies.
# Windows always requires AVX2. CI never adds host-specific CPU flags.

include(CheckCompilerFlag)
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
            cpu_run_result cpu_compile_result SOURCES "${CMAKE_CURRENT_LIST_DIR}/DetectNativeCpu.cpp" NO_CACHE
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
        # Native targeting must not fuse separately rounded DSP multiply/add operations.
        add_compile_options("$<${cpu_languages}:/clang:-ffp-contract=off>")
    else()
        add_compile_options("$<$<AND:${cpu_languages},$<CONFIG:Release>>:/fp:precise>")
    endif()
    message(STATUS "AudioBatch Windows CPU baseline: AVX2")
elseif(use_native)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
        set(native_flags -mcpu=native -mtune=native)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i[3-6]86)$")
        set(native_flags -march=native -mtune=native)
        # Native x86 targeting introduces FMA, which must not change separate multiply/add rounding.
        add_compile_options("$<$<AND:${cpu_languages},$<CONFIG:Release>>:-ffp-contract=off>")
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
endif()

if(NOT AUDIOBATCH_NATIVE_CPU)
    message(STATUS "AudioBatch native CPU targeting: disabled by AUDIOBATCH_NATIVE_CPU")
endif()

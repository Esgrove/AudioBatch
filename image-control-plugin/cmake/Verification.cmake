# Instrument only the standalone ImageControl build, including its JUCE modules and unit-test executable.
# The application projects are not added to this CMake tree.
include_guard(GLOBAL)

set(IMAGE_CONTROL_SANITIZE "" CACHE STRING "ImageControl sanitizers: address,undefined or thread (macOS)")
set_property(CACHE IMAGE_CONTROL_SANITIZE PROPERTY STRINGS "" "address,undefined" "thread")
option(IMAGE_CONTROL_COVERAGE "Instrument ImageControl for Clang source coverage (macOS)" OFF)

if(IMAGE_CONTROL_SANITIZE OR IMAGE_CONTROL_COVERAGE)
    if(NOT APPLE OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "ImageControl sanitizer and coverage configurations require Clang on macOS")
    endif()
    if(IMAGE_CONTROL_SANITIZE AND IMAGE_CONTROL_COVERAGE)
        message(FATAL_ERROR "ImageControl sanitizers and coverage require separate build directories")
    endif()
    # Instrumentation must not depend on Release IPO or host-specific code generation.
    set(AUDIOBATCH_LTO OFF)
    set(AUDIOBATCH_NATIVE_CPU OFF)
endif()

if(IMAGE_CONTROL_SANITIZE)
    if(NOT IMAGE_CONTROL_SANITIZE STREQUAL "address,undefined" AND NOT IMAGE_CONTROL_SANITIZE STREQUAL "thread")
        message(FATAL_ERROR "IMAGE_CONTROL_SANITIZE must be address,undefined or thread")
    endif()
    add_compile_options(-fsanitize=${IMAGE_CONTROL_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${IMAGE_CONTROL_SANITIZE})
endif()

if(IMAGE_CONTROL_COVERAGE)
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
    add_link_options(-fprofile-instr-generate)
endif()

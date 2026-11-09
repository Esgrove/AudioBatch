# Instrument only the standalone ImageControl build, including its JUCE modules and unit-test executable.
# The application projects are not added to this CMake tree.
include_guard(GLOBAL)

set(IMAGE_CONTROL_SANITIZE "" CACHE STRING "ImageControl sanitizers: address,undefined, thread, or realtime (macOS)")
set_property(
    CACHE IMAGE_CONTROL_SANITIZE
    PROPERTY
        STRINGS
        ""
        "address,undefined"
        "thread"
        "realtime"
)
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
    if(NOT IMAGE_CONTROL_SANITIZE STREQUAL "address,undefined"
       AND NOT IMAGE_CONTROL_SANITIZE STREQUAL "thread"
       AND NOT IMAGE_CONTROL_SANITIZE STREQUAL "realtime"
    )
        message(FATAL_ERROR "IMAGE_CONTROL_SANITIZE must be address,undefined, thread, or realtime")
    endif()
    if(IMAGE_CONTROL_SANITIZE STREQUAL "realtime")
        if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            message(FATAL_ERROR "ImageControl RTSan requires upstream Homebrew LLVM Clang, not Apple Clang")
        endif()
        if(NOT IMAGE_CONTROL_BUILD_TESTS)
            message(FATAL_ERROR "ImageControl RTSan requires IMAGE_CONTROL_BUILD_TESTS=ON")
        endif()
        include(CheckCXXSourceCompiles)
        include(CMakePushCheckState)
        cmake_push_check_state(RESET)
        set(CMAKE_REQUIRED_FLAGS "-fsanitize=realtime -Werror=unknown-attributes")
        set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=realtime)
        check_cxx_source_compiles(
            "#if !__has_feature(realtime_sanitizer) || !__has_attribute(nonblocking)
             #error RTSan and the nonblocking attribute are required
             #endif
             int callback() noexcept [[clang::nonblocking]] { return 42; }
             int main() { return callback() == 42 ? 0 : 1; }"
            IMAGE_CONTROL_HAS_REALTIME_SANITIZER
        )
        cmake_pop_check_state()
        if(NOT IMAGE_CONTROL_HAS_REALTIME_SANITIZER)
            message(FATAL_ERROR "Clang cannot compile and startup-link -fsanitize=realtime with [[clang::nonblocking]]")
        endif()
        # Host and loadable bundle must resolve exactly the same compiler runtime, without DYLD injection.
        execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" -print-runtime-dir
            OUTPUT_VARIABLE IMAGE_CONTROL_REALTIME_RUNTIME_DIRECTORY
            OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY
        )
        if(NOT EXISTS "${IMAGE_CONTROL_REALTIME_RUNTIME_DIRECTORY}/libclang_rt.rtsan_osx_dynamic.dylib")
            message(FATAL_ERROR "The selected Clang has no dynamic macOS RTSan runtime")
        endif()
    endif()
    add_compile_options(-fsanitize=${IMAGE_CONTROL_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${IMAGE_CONTROL_SANITIZE})
endif()

if(IMAGE_CONTROL_COVERAGE)
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
    add_link_options(-fprofile-instr-generate)
endif()

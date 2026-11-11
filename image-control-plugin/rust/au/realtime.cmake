# The standalone AU RTSan host is deliberately separate from functional AU qualification.
# The caller must instrument the upstream wrapper and matching Rust archive as well as this executable.
option(IMAGE_CONTROL_AU_RTSAN "Build the startup-linked native AU RealtimeSanitizer host" OFF)
if(NOT IMAGE_CONTROL_AU_RTSAN)
    return()
endif()
if(NOT APPLE OR NOT CMAKE_OBJCXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "IMAGE_CONTROL_AU_RTSAN requires native macOS LLVM Clang with RealtimeSanitizer")
endif()

add_executable(ImageControlRSAURealtime "${CMAKE_CURRENT_LIST_DIR}/tests/AURealtime.mm")
target_compile_definitions(
    ImageControlRSAURealtime PRIVATE FMT_HEADER_ONLY IMAGE_CONTROL_VERSION="${IMAGE_CONTROL_VERSION}"
)
target_include_directories(ImageControlRSAURealtime SYSTEM PRIVATE "${image_control_clap_wrapper_SOURCE_DIR}/libs/fmt")
set_target_properties(
    ImageControlRSAURealtime
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        OBJCXX_STANDARD 23
        OBJCXX_STANDARD_REQUIRED ON
        OBJCXX_EXTENSIONS OFF
)
target_compile_options(
    ImageControlRSAURealtime
    PRIVATE
        -fsanitize=realtime
        -g
        -fno-omit-frame-pointer
        -fobjc-arc
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
# Compiler-driver linkage selects exactly the runtime matching this host's instrumenting compiler.
target_link_options(ImageControlRSAURealtime PRIVATE -fsanitize=realtime)
target_link_libraries(
    ImageControlRSAURealtime PRIVATE "-framework AudioToolbox" "-framework AudioUnit" "-framework CoreFoundation"
)
add_dependencies(ImageControlRSAURealtime ImageControlRSAU)

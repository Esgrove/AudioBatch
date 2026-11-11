# ImageControl RS AUv2 statically links the Rust CLAP entry without installing either format.
cmake_minimum_required(VERSION 4.2)

set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum macOS version")
project(ImageControlRSAU LANGUAGES C CXX OBJC OBJCXX)

if(NOT APPLE OR NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR "ImageControlRSAU requires macOS")
endif()
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET VERSION_EQUAL "15.0")
    message(FATAL_ERROR "The AU and Rust archive must both target macOS 15.0")
endif()
if(NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|Release)$" OR CMAKE_CONFIGURATION_TYPES)
    message(FATAL_ERROR "Use a single-configuration generator with Debug or Release matching the Rust profile")
endif()
if(NOT CMAKE_OSX_ARCHITECTURES MATCHES "^(arm64|x86_64)$")
    message(FATAL_ERROR "Set CMAKE_OSX_ARCHITECTURES to the single architecture of the Rust archive")
endif()

set(IMAGE_CONTROL_RUST_LIBRARY "" CACHE FILEPATH "Absolute matching-profile Rust static library exporting clap_entry")
set(IMAGE_CONTROL_VERSION "" CACHE STRING "ImageControl RS semantic version")
if(NOT IS_ABSOLUTE "${IMAGE_CONTROL_RUST_LIBRARY}" OR NOT EXISTS "${IMAGE_CONTROL_RUST_LIBRARY}")
    message(FATAL_ERROR "IMAGE_CONTROL_RUST_LIBRARY must name an existing absolute libimage_control_plugin_rs.a path")
endif()
get_filename_component(IMAGE_CONTROL_RUST_LIBRARY_NAME "${IMAGE_CONTROL_RUST_LIBRARY}" NAME)
if(NOT IMAGE_CONTROL_RUST_LIBRARY_NAME STREQUAL "libimage_control_plugin_rs.a")
    message(FATAL_ERROR "IMAGE_CONTROL_RUST_LIBRARY must be libimage_control_plugin_rs.a")
endif()
if(NOT IMAGE_CONTROL_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "IMAGE_CONTROL_VERSION must be the Rust package's numeric major.minor.patch version")
endif()

# Architecture is inspectable in the archive. The caller owns matching Cargo's profile and deployment target.
execute_process(
    COMMAND /usr/bin/lipo -archs "${IMAGE_CONTROL_RUST_LIBRARY}"
    OUTPUT_VARIABLE IMAGE_CONTROL_ARCHIVE_ARCHITECTURES
    OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY
)
if(NOT IMAGE_CONTROL_ARCHIVE_ARCHITECTURES STREQUAL CMAKE_OSX_ARCHITECTURES)
    message(FATAL_ERROR "Rust archive architecture ${IMAGE_CONTROL_ARCHIVE_ARCHITECTURES} does not match CMake")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 23)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_SKIP_INSTALL_RULES ON)

include(FetchContent)
# CLAP 1.2.10 and AudioUnitSDK 1.1.0 match this wrapper revision's upstream SDK selections.
# Full commits were resolved with the GitHub API, including the annotated Apple release tag.
FetchContent_Declare(
    image_control_clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG 195b42a004144fab0b3cf95e9c067187d15365b7 GIT_SUBMODULES ""
    SOURCE_SUBDIR image-control-no-subdirectory
)
FetchContent_Declare(
    image_control_audio_unit_sdk
    GIT_REPOSITORY https://github.com/apple/AudioUnitSDK.git
    GIT_TAG 53a9a2008aae7fb1b0a9f093dd523b9b12f6c0d9 GIT_SUBMODULES ""
    SOURCE_SUBDIR image-control-no-subdirectory
)
FetchContent_Declare(
    image_control_clap_wrapper
    GIT_REPOSITORY https://github.com/free-audio/clap-wrapper.git
    GIT_TAG ddba261acdb421e9963959aa970a10876515aa6a GIT_SUBMODULES ""
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(image_control_clap image_control_audio_unit_sdk)
# CLAP is header-only. Own its interface target instead of enabling the SDK's installation rules.
add_library(clap INTERFACE)
target_include_directories(clap SYSTEM INTERFACE "${image_control_clap_SOURCE_DIR}/include")
set(CLAP_SDK_ROOT "${image_control_clap_SOURCE_DIR}")
set(AUDIOUNIT_SDK_ROOT "${image_control_audio_unit_sdk_SOURCE_DIR}")
set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES OFF CACHE BOOL "Use only the pinned SDKs" FORCE)
set(CLAP_WRAPPER_COPY_AFTER_BUILD OFF CACHE BOOL "Never install from the AU build" FORCE)
set(CLAP_WRAPPER_BUILD_TESTS OFF CACHE BOOL "No upstream sample plugins" FORCE)
FetchContent_MakeAvailable(image_control_clap_wrapper)

add_library(ImageControlRust STATIC IMPORTED GLOBAL)
set_target_properties(ImageControlRust PROPERTIES IMPORTED_LOCATION "${IMAGE_CONTROL_RUST_LIBRARY}")

add_library(ImageControlRSAU MODULE)
target_link_libraries(ImageControlRSAU PRIVATE ImageControlRust)
# Force retention and export of the data symbol used by upstream's CFBundle self-resolution.
# Do not set STATICALLY_LINKED_CLAP_ENTRY on the shared helper: its build-time generator must remain plugin-free.
target_link_options(ImageControlRSAU PRIVATE "LINKER:-u,_clap_entry")
target_add_auv2_wrapper(
    TARGET
    ImageControlRSAU
    OUTPUT_NAME
    "ImageControl RS"
    BUNDLE_IDENTIFIER
    com.esgrove.imagecontrol-rs-own.au
    BUNDLE_VERSION
    "${IMAGE_CONTROL_VERSION}"
    MANUFACTURER_NAME
    Esgrove
    MANUFACTURER_CODE
    Esgr
    SUBTYPE_CODE
    Egir
    INSTRUMENT_TYPE
    aufx
    PREFER_CMAKE_AUV2_CONFIGURATION
    TRUE
    DICTIONARY_STREAM_FORMAT
    WRAPPER
)
set_target_properties(
    ImageControlRSAU
    PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO
        XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED NO
)
# Rust's native dependencies are system frameworks, not another plugin or a build-tree dylib.
target_link_libraries(
    ImageControlRSAU
    PRIVATE
        "-framework Accelerate"
        "-framework AudioUnit"
        "-framework CoreAudio"
        "-framework CoreGraphics"
        "-framework CoreVideo"
        "-framework IOKit"
        "-framework Metal"
        "-framework QuartzCore"
        "-framework Security"
        "-framework SystemConfiguration"
        "-lobjc"
        "-liconv"
)
add_custom_command(
    TARGET ImageControlRSAU
    POST_BUILD
    # Upstream appends .component to its configured identifier. Keep our persisted identity exact before signing.
    COMMAND /usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier com.esgrove.imagecontrol-rs-own.au"
            "$<TARGET_BUNDLE_DIR:ImageControlRSAU>/Contents/Info.plist"
    COMMAND /usr/bin/codesign --force --sign - --timestamp=none "$<TARGET_BUNDLE_DIR:ImageControlRSAU>"
    COMMAND /usr/bin/codesign --verify --deep --strict "$<TARGET_BUNDLE_DIR:ImageControlRSAU>"
    VERBATIM
)

# The exact path is stable for xtask and CTest. No registration service, plugin folders, or auval is involved.
set(IMAGE_CONTROL_AU_BUNDLE "${CMAKE_BINARY_DIR}/ImageControl RS.component" CACHE INTERNAL "Build-tree AU bundle")
message(STATUS "ImageControl RS AU bundle: ${IMAGE_CONTROL_AU_BUNDLE}")
message(STATUS "Rust profile must match CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")

option(IMAGE_CONTROL_AU_BUILD_TESTS "Build the isolated native AU qualification host" ON)
if(IMAGE_CONTROL_AU_BUILD_TESTS)
    enable_testing()
    find_program(IMAGE_CONTROL_UV uv REQUIRED)
    add_executable(ImageControlRSAUHost tests/AUHost.mm)
    target_compile_definitions(
        ImageControlRSAUHost PRIVATE FMT_HEADER_ONLY IMAGE_CONTROL_VERSION="${IMAGE_CONTROL_VERSION}"
    )
    target_compile_options(ImageControlRSAUHost PRIVATE -fobjc-arc)
    target_include_directories(ImageControlRSAUHost SYSTEM PRIVATE "${image_control_clap_wrapper_SOURCE_DIR}/libs/fmt")
    target_link_libraries(
        ImageControlRSAUHost PRIVATE "-framework AudioToolbox" "-framework AudioUnit" "-framework AppKit"
                                     "-framework CoreFoundation"
    )
    target_compile_options(
        ImageControlRSAUHost
        PRIVATE
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
    add_dependencies(ImageControlRSAUHost ImageControlRSAU)
    add_test(NAME ImageControlRSAUHost COMMAND ImageControlRSAUHost --bundle "${IMAGE_CONTROL_AU_BUNDLE}")
    set_tests_properties(ImageControlRSAUHost PROPERTIES TIMEOUT 90 LABELS "au;qualification")
    add_test(
        NAME ImageControlRSAUUpstreamAudit
        COMMAND "${IMAGE_CONTROL_UV}" run --script "${CMAKE_CURRENT_SOURCE_DIR}/tests/audit_upstream.py" --source
                "${image_control_clap_wrapper_SOURCE_DIR}"
    )
    set_tests_properties(ImageControlRSAUUpstreamAudit PROPERTIES TIMEOUT 30 LABELS "au;dependency-audit")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/realtime.cmake")

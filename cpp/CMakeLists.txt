# AudioBatch
# Akseli Lukkarila
# 2023-2026

cmake_minimum_required(VERSION 4.2)

# Select the same SDK for compiler detection and dependency discovery, including builds without a preset.
# Keep explicit SDK choices, including SDKROOT from the environment.
if(CMAKE_HOST_APPLE
   AND NOT CMAKE_OSX_SYSROOT
   AND "$ENV{SDKROOT}" STREQUAL ""
)
    set(CMAKE_OSX_SYSROOT macosx CACHE STRING "macOS SDK" FORCE)
endif()

project(
    "AudioBatch"
    VERSION 1.2.0
    DESCRIPTION "Batch analyze and process audio files"
    HOMEPAGE_URL "https://github.com/Esgrove/AudioBatch"
    LANGUAGES CXX C
)

include(FetchContent)
include(GNUInstallDirs)
option(BUILD_TESTING "Build AudioBatch tests" ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
# Export compile commands for clang-tidy
# Note: only works with makefile and ninja generators
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

message(STATUS "CMake Version: ${CMAKE_VERSION}")
message(STATUS "CMake source dir: ${CMAKE_SOURCE_DIR}")

# Git hash
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
# Git branch
execute_process(
    COMMAND git branch --show-current
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE GIT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
# Latest version tag, such as v1.0.0, annotated or not
execute_process(
    COMMAND git describe --tags --abbrev=0 --match "v[0-9]*"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE GIT_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
)
# Build number: the commits since the latest version tag, or all commits when there is no tag yet
if(GIT_TAG)
    set(BUILD_NUMBER_RANGE "${GIT_TAG}..HEAD")
else()
    set(BUILD_NUMBER_RANGE "HEAD")
endif()
execute_process(
    COMMAND git rev-list --count ${BUILD_NUMBER_RANGE}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE BUILD_NUMBER
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT BUILD_NUMBER)
    set(BUILD_NUMBER 0)
endif()

# The git values above are only read when CMake configures,
# so reconfigure when HEAD moves, the current branch gets a new commit, or a tag is added or moved.
# Without this, an existing build directory keeps the commit and build number it was first configured with.
# The paths come from git, so they also resolve in a worktree, where .git is a file.
execute_process(
    COMMAND git symbolic-ref -q HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE GIT_HEAD_REF
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
)
foreach(git_path HEAD packed-refs refs/tags ${GIT_HEAD_REF})
    execute_process(
        COMMAND git rev-parse --path-format=absolute --git-path ${git_path}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE git_file
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
    )
    # The Visual Studio generators only accept files as custom build dependencies and warn (MSB8064) about a directory.
    if(git_file
       AND EXISTS "${git_file}"
       AND NOT (IS_DIRECTORY "${git_file}" AND CMAKE_GENERATOR MATCHES "Visual Studio")
    )
        set_property(
            DIRECTORY
            APPEND
            PROPERTY CMAKE_CONFIGURE_DEPENDS "${git_file}"
        )
    endif()
endforeach()

# Date and time
string(TIMESTAMP DATE "%Y-%m-%d" UTC)
string(
    TIMESTAMP
    TIMESTAMP "%Y-%m-%d_%H%M"
    UTC
)

set(APP_BUILD_VERSION "${CMAKE_PROJECT_VERSION}.${BUILD_NUMBER}")
set(APP_BUILD_NAME "${CMAKE_PROJECT_NAME}_${APP_BUILD_VERSION}_${GIT_HASH}_${TIMESTAMP}")

# Version text printed by --version: version, date, commit, and branch.
# The branch is left out when it is empty, for example on a detached HEAD in CI.
set(APP_VERSION_STRING "${APP_BUILD_VERSION} ${DATE} ${GIT_HASH}")
if(GIT_BRANCH)
    string(APPEND APP_VERSION_STRING " ${GIT_BRANCH}")
endif()

message(STATUS "Build name: ${APP_BUILD_NAME}")

# The version constants go into a generated header instead of compile definitions,
# so a new commit or date only rebuilds the files that include it.
# configure_file leaves the header untouched when its content does not change.
set(AUDIOBATCH_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file("src/utils/version.h.in" "${AUDIOBATCH_GENERATED_DIR}/utils/version.h" @ONLY)

# The icons, fonts and test fixtures live at the repository root, shared with the Rust port.
cmake_path(SET AUDIOBATCH_ASSETS_DIR NORMALIZE "${CMAKE_SOURCE_DIR}/../assets")
cmake_path(SET AUDIOBATCH_FIXTURES_DIR NORMALIZE "${CMAKE_SOURCE_DIR}/../fixtures")

if(WIN32)
    set(ICON_FILE "${AUDIOBATCH_ASSETS_DIR}/icon_win.png")
else()
    # macOS settings
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum macOS version")
    # Xcode: Disable automatic build scheme generation.
    # Instead, explicitly specify which targets require schemes.
    set(CMAKE_XCODE_GENERATE_SCHEME OFF)
    set(CMAKE_XCODE_ATTRIBUTE_DEAD_CODE_STRIPPING YES)
    set(ICON_FILE "${AUDIOBATCH_ASSETS_DIR}/icon_mac.png")
endif()

if(APPLE)
    # Keep the PNG fallback for older macOS versions alongside the layered Icon Composer artwork.
    set(ICON_COMPOSER_BUNDLE "${AUDIOBATCH_ASSETS_DIR}/icon_mac.icon")
endif()

# Use ccache for compilation caching when available.
# Must be set before any targets are created so it also covers JUCE and the fetched dependencies.
# Only enabled for Ninja and Makefile generators,
# since Xcode and Visual Studio generators need special setup that is not worth the complexity here.
if(CMAKE_GENERATOR MATCHES "Ninja|Unix Makefiles" AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
        set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    endif()
endif()

# Comma separated sanitizer list such as "address,undefined", empty to disable.
# The flags are global so JUCE and the fetched dependencies are instrumented the same way as project code.
# Must be set before any targets are made. LeakSanitizer is not supported on Apple Silicon.
set(AUDIOBATCH_SANITIZE "" CACHE STRING "Comma separated list of sanitizers to enable, for example address,undefined")

if(AUDIOBATCH_SANITIZE)
    if(MSVC)
        if(NOT AUDIOBATCH_SANITIZE STREQUAL "address")
            message(FATAL_ERROR "MSVC only supports AUDIOBATCH_SANITIZE=address")
        endif()
        add_compile_options(/fsanitize=address)
        # AddressSanitizer cannot be combined with incremental linking
        # or with the runtime checks that Debug builds enable by default.
        set(CMAKE_MSVC_RUNTIME_CHECKS "")
        add_link_options(/INCREMENTAL:NO)
        # Instrumented executables load the AddressSanitizer runtime that ships next to the compiler.
        cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH msvcBinaryDirectory)
        file(GLOB AUDIOBATCH_ASAN_RUNTIME "${msvcBinaryDirectory}/clang_rt.asan*_dynamic-x86_64.dll")
        if(NOT AUDIOBATCH_ASAN_RUNTIME)
            message(FATAL_ERROR "Could not find the AddressSanitizer runtime in ${msvcBinaryDirectory}")
        endif()
    else()
        # Without -fno-sanitize-recover, UndefinedBehaviorSanitizer prints a finding and continues,
        # so the test run would still pass.
        add_compile_options(-fsanitize=${AUDIOBATCH_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
        add_link_options(-fsanitize=${AUDIOBATCH_SANITIZE})
    endif()
endif()

# libc++ hardening turns out of range access through std::span, std::optional, std::vector and similar into a trap.
# Debug builds, which include the sanitizer builds, use the extensive checks,
# and the other configurations keep the cheap checks that are suitable for release builds.
# MSVC STL hardening adds the same cheap checks to every configuration,
# on top of the iterator debugging that Debug builds already have.
# The mode is global so JUCE and the fetched dependencies use the same checks as project code.
if(APPLE)
    add_compile_definitions(
        _LIBCPP_HARDENING_MODE=$<IF:$<CONFIG:Debug>,_LIBCPP_HARDENING_MODE_EXTENSIVE,_LIBCPP_HARDENING_MODE_FAST>
    )
elseif(MSVC)
    add_compile_definitions(_MSVC_STL_HARDENING=1)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
endif()

# Adds all the module sources so they appear correctly in the IDE
# Must be set before JUCE is added as a sub-dir (or any targets are made)
# https://github.com/juce-framework/JUCE/commit/6b1b4cf7f6b1008db44411f2c8887d71a3348889
set_property(GLOBAL PROPERTY USE_FOLDERS YES)

# Create a "/Modules" directory in the IDE with the JUCE Module code
option(JUCE_ENABLE_MODULE_SOURCE_GROUPS "Show all module sources in IDE projects" ON)

add_subdirectory(JUCE)

find_package(SQLite3 QUIET)

if(SQLite3_FOUND)
    set(AUDIOBATCH_SQLITE_TARGET SQLite3::SQLite3)
else()
    # The amalgamation has no CMakeLists.txt, so FetchContent_MakeAvailable only downloads and extracts it.
    FetchContent_Declare(
        sqlite3
        URL https://www.sqlite.org/2026/sqlite-amalgamation-3510300.zip
        URL_HASH SHA256=acb1e6f5d832484bf6d32b681e858c38add8b2acdfd42ac5df24b8afb46552b4
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(sqlite3)

    add_library(AudioBatchSQLite STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)

    target_compile_definitions(AudioBatchSQLite PRIVATE SQLITE_DQS=0 SQLITE_OMIT_LOAD_EXTENSION=1 SQLITE_THREADSAFE=1)

    target_include_directories(AudioBatchSQLite PUBLIC ${sqlite3_SOURCE_DIR})

    set(AUDIOBATCH_SQLITE_TARGET AudioBatchSQLite)
endif()

# On Windows, prefer the static zlib import library
# so taglib does not introduce a runtime dependency on zlib.dll.
# This must be set before taglib is configured, since taglib calls find_package(ZLIB).
# On macOS the system provides no static zlib, so requiring one would make find_package(ZLIB) fail
# and taglib would silently lose support for compressed ID3v2 frames.
# Linking the system libz dynamically is the standard approach there.
if(WIN32)
    set(ZLIB_USE_STATIC_LIBS ON)
endif()
find_package(ZLIB QUIET)

# Apple SDK headers are already found through the compiler's sysroot.
# FindZLIB and FindSQLite3 can return the SDK's unversioned symlink as an explicit system include,
# which puts the C headers before libc++ when the compiler uses a different SDK path.
# Omit only SDK root includes, preserving include directories for separately installed libraries.
if(APPLE)
    foreach(system_target IN ITEMS SQLite3::SQLite3 ZLIB::ZLIB)
        if(TARGET ${system_target})
            get_target_property(system_includes ${system_target} INTERFACE_INCLUDE_DIRECTORIES)
            if(system_includes)
                list(FILTER system_includes EXCLUDE REGEX "\\.sdk/usr/include/?$")
                set_target_properties(${system_target} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${system_includes}")
            endif()
        endif()
    endforeach()
endif()

# The dependencies are pinned to the commit of their release tag, because a tag can be moved.
# A shallow clone needs a branch or tag name, so these are full clones.
# TagLib is fetched with git instead of an archive, because it has utfcpp as a submodule.
FetchContent_Declare(
    taglib
    GIT_REPOSITORY https://github.com/taglib/taglib.git
    # v2.3
    GIT_TAG 1b94b93762636ebe5733180c3e825be4621e4c7f
)

# Configure taglib build options before populating, so they apply on the first configure
# and remain in effect on subsequent re-configures (when the source is already populated).
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_BINDINGS OFF CACHE BOOL "" FORCE)
set(VISIBILITY_HIDDEN ON CACHE BOOL "" FORCE)

# FetchContent_MakeAvailable both populates the source (when needed)
# and calls add_subdirectory every time CMake runs, so the `tag` target is always defined for the rest of this build.
block(PROPAGATE taglib_SOURCE_DIR taglib_BINARY_DIR)
set(BUILD_TESTING OFF)
FetchContent_MakeAvailable(taglib)
endblock()

# TagLib does not propagate its build-tree include directories to consumers,
# so add them explicitly so we can include its public headers.
target_include_directories(
    tag
    PUBLIC
        $<BUILD_INTERFACE:${taglib_BINARY_DIR}>
        $<BUILD_INTERFACE:${taglib_SOURCE_DIR}>
        $<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib>
        $<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/ape>
        $<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/flac>
        $<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/mp4>
        $<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/mpeg/id3v1>
        $<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/mpeg/id3v2/frames>
        $<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/mpeg/id3v2>
        $<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/mpeg>
        $<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/ogg/flac>
        $<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/ogg/opus>
        $<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/ogg/vorbis>
        $<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/ogg>
        $<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/riff/aiff>
        $<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/riff/wav>
        $<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/riff>
        $<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/toolkit>
)

# Treat TagLib headers as system headers for consumers, not its own sources,
# so they are exempt from this project's warnings-as-errors flags.
set_target_properties(tag PROPERTIES SYSTEM ON)

# Only the library subdirectory is added, which leaves out the tests and the install rules of the top level project.
# The patch step turns the gates that ebur128_init writes into globals into constants,
# so analysis workers can create meters concurrently. See cmake/PatchLibebur128.cmake.
FetchContent_Declare(
    libebur128
    GIT_REPOSITORY https://github.com/jiixyj/libebur128.git
    # v1.2.6
    GIT_TAG 67b33abe1558160ed76ada1322329b0e9e058b02
    SOURCE_SUBDIR ebur128
    EXCLUDE_FROM_ALL
    PATCH_COMMAND ${CMAKE_COMMAND} -DEBUR128_SOURCE=<SOURCE_DIR>/ebur128/ebur128.c -P
                  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchLibebur128.cmake
)
FetchContent_MakeAvailable(libebur128)

target_include_directories(ebur128 PUBLIC ${libebur128_SOURCE_DIR}/ebur128)

# Suppress bogus SSE2/FTZ #warning from libebur128 on ARM64
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i[3-6]86")
    target_compile_options(ebur128 PRIVATE -Wno-\#warnings)
endif()

# SYSTEM marks the fmt include directories as system includes,
# so its headers are exempt from this project's warnings-as-errors flags.
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    # 12.2.0
    GIT_TAG 1be298e1bd68957e4cd352e1f676f00e07dcfb57
    SYSTEM
)

# Configure fmt build options before populating,
# so they apply on the first configure and on subsequent re-configures.
set(FMT_DOC OFF CACHE BOOL "" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(FMT_FUZZ OFF CACHE BOOL "" FORCE)
# fmt enables its C++ module library by default with Ninja and upstream Clang, and the project only uses the headers.
set(FMT_MODULE OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(fmt)

juce_add_gui_app(
    AudioBatch
    BUNDLE_ID com.esgrove.audiobatch
    COMPANY_NAME "Esgrove"
    COMPANY_WEBSITE "https://github.com/esgrove"
    ICON_BIG ${ICON_FILE} ICON_COMPOSER_BUNDLE "${ICON_COMPOSER_BUNDLE}"
    PRODUCT_NAME "AudioBatch"
    VERSION ${APP_BUILD_VERSION}
    PLIST_TO_MERGE
        "<plist version=\"1.0\">
        <dict>
            <key>LSApplicationCategoryType</key>
            <string>public.app-category.developer-tools</string>
        </dict>
        </plist>"
)

juce_add_console_app(
    AudioBatchCli
    COMPANY_NAME "Esgrove"
    COMPANY_WEBSITE "https://github.com/esgrove"
    PRODUCT_NAME "audiobatch"
    VERSION ${APP_BUILD_VERSION}
)

juce_generate_juce_header(AudioBatch)
juce_generate_juce_header(AudioBatchCli)

# Sources shared by the GUI, the CLI and the tests.
# Paths stay relative to the source directory,
# which audiobatch_set_source_warnings relies on.
set(AUDIOBATCH_CORE_SOURCES
    "src/analysis/AnalysisCache.cpp"
    "src/analysis/AnalysisCache.h"
    "src/analysis/AnalysisCacheSchema.cpp"
    "src/analysis/AnalysisCacheSql.h"
    "src/analysis/AnalysisCoordinator.cpp"
    "src/analysis/AnalysisCoordinator.h"
    "src/analysis/AudioAnalysis.cpp"
    "src/analysis/AudioAnalysis.h"
    "src/analysis/AudioAnalysisFormatting.cpp"
    "src/analysis/AudioAnalysisFormatting.h"
    "src/analysis/AudioAnalysisTypes.h"
    "src/analysis/BitDepthDetection.cpp"
    "src/analysis/BitDepthDetection.h"
    "src/analysis/DecodeFailureTracker.h"
    "src/analysis/OverloadHistory.cpp"
    "src/analysis/OverloadHistory.h"
    "src/analysis/Sqlite.cpp"
    "src/analysis/Sqlite.h"
    "src/analysis/SqliteTransaction.cpp"
    "src/analysis/SqliteTransaction.h"
    "src/metadata/AudioMetadata.cpp"
    "src/metadata/AudioMetadata.h"
    "src/normalization/AudioNormalization.cpp"
    "src/normalization/AudioNormalization.h"
    "src/normalization/NormalizationFormatSupport.cpp"
    "src/normalization/NormalizeCoordinator.cpp"
    "src/normalization/NormalizeCoordinator.h"
    "src/plugins/PluginScanner.cpp"
    "src/plugins/PluginScanner.h"
    "src/serato/SeratoBytes.h"
    "src/serato/SeratoOverview.cpp"
    "src/serato/SeratoOverview.h"
    "src/serato/SeratoTagCache.cpp"
    "src/serato/SeratoTagCache.h"
    "src/serato/SeratoTagReader.cpp"
    "src/serato/SeratoTagReader.h"
    "src/serato/SeratoTags.cpp"
    "src/serato/SeratoTags.h"
    "src/serato/SeratoTagTypes.h"
    "src/utils/AppInfo.cpp"
    "src/utils/AppInfo.h"
    "src/utils/AudioFormatUtils.cpp"
    "src/utils/AudioFormatUtils.h"
    "src/utils/BackgroundRunner.cpp"
    "src/utils/BackgroundRunner.h"
    "src/utils/ConsolePrint.cpp"
    "src/utils/ConsolePrint.h"
    "src/utils/FileUtils.cpp"
    "src/utils/FileUtils.h"
    "src/utils/GuiUtils.cpp"
    "src/utils/GuiUtils.h"
    "src/utils/Log.cpp"
    "src/utils/Log.h"
    "src/utils/MessageThreadBatcher.cpp"
    "src/utils/MessageThreadBatcher.h"
    "src/utils/StringFormat.h"
    "src/utils/ThreadUtils.cpp"
    "src/utils/ThreadUtils.h"
    "src/utils/version.h.in"
)

# CLI front end sources, also linked into the tests.
# cmake-format: off
set(AUDIOBATCH_CLI_SOURCES
    "src/cli/AudioAnalysisCli.cpp"
    "src/cli/AudioAnalysisCli.h"
    "src/cli/AudioAnalysisCliOutput.cpp"
    "src/cli/AudioAnalysisCliOutput.h"
    "src/cli/AudioAnalysisCliParse.cpp"
    "src/cli/CliOptions.h"
    "src/cli/PluginListCli.cpp"
    "src/cli/PluginListCli.h"
    "src/cli/ShellCompletion.cpp"
    "src/cli/ShellCompletion.h"
    "src/utils/ConsoleProgress.cpp"
    "src/utils/ConsoleProgress.h"
)
# cmake-format: on

target_sources(
    AudioBatch
    PRIVATE
        ${AUDIOBATCH_CORE_SOURCES}
        "src/gui/ArtworkLoader.cpp"
        "src/gui/ArtworkLoader.h"
        "src/gui/AudioBatchComponent.cpp"
        "src/gui/AudioBatchComponent.h"
        "src/gui/AudioBatchComponentActions.cpp"
        "src/gui/AudioBatchComponentConstants.h"
        "src/gui/AudioBatchComponentLayout.cpp"
        "src/gui/AudioBatchComponentOperations.cpp"
        "src/gui/AudioBatchComponentPreview.cpp"
        "src/gui/AudioBatchComponentRoots.cpp"
        "src/gui/AudioBatchComponentState.h"
        "src/gui/AudioFileTableModel.cpp"
        "src/gui/AudioFileTableModel.h"
        "src/gui/AudioPreviewPlayer.cpp"
        "src/gui/AudioPreviewPlayer.h"
        "src/gui/BackgroundWorkController.cpp"
        "src/gui/BackgroundWorkController.h"
        "src/gui/components/AudioInfoPanel.cpp"
        "src/gui/components/AudioInfoPanel.h"
        "src/gui/components/AudioSettingsComponent.cpp"
        "src/gui/components/AudioSettingsComponent.h"
        "src/gui/components/CustomLookAndFeel.cpp"
        "src/gui/components/CustomLookAndFeel.h"
        "src/gui/components/IntervalStepSlider.h"
        "src/gui/components/NormalizationTargetControl.cpp"
        "src/gui/components/NormalizationTargetControl.h"
        "src/gui/components/PluginChainEditor.cpp"
        "src/gui/components/PluginChainEditor.h"
        "src/gui/components/PreviewWorkspace.cpp"
        "src/gui/components/PreviewWorkspace.h"
        "src/gui/components/StatusBar.cpp"
        "src/gui/components/StatusBar.h"
        "src/gui/components/ThumbnailComponent.cpp"
        "src/gui/components/ThumbnailComponent.h"
        "src/gui/FileActivityTracker.cpp"
        "src/gui/FileActivityTracker.h"
        "src/gui/GuiLayout.h"
        "src/gui/LiveChain.cpp"
        "src/gui/LiveChain.h"
        "src/gui/LiveChainPreview.cpp"
        "src/gui/LiveChainPresets.cpp"
        "src/gui/LiveChainWindows.cpp"
        "src/gui/Main.cpp"
        "src/gui/MainWindow.cpp"
        "src/gui/MainWindow.h"
        "src/gui/PreviewController.cpp"
        "src/gui/PreviewController.h"
        "src/gui/ResultsModel.cpp"
        "src/gui/ResultsModel.h"
        "src/gui/ResultsSorting.cpp"
        "src/gui/ResultsSorting.h"
        "src/plugins/PluginLibrary.cpp"
        "src/plugins/PluginLibrary.h"
        "src/plugins/PluginPreviewSource.cpp"
        "src/plugins/PluginPreviewSource.h"
        "src/plugins/PluginChainPreparation.cpp"
        "src/plugins/PluginChainPreparation.h"
        "src/plugins/PluginChainPresets.cpp"
        "src/plugins/PluginChainPresets.h"
        "src/plugins/PluginProcessing.cpp"
        "src/plugins/PluginProcessing.h"
        "src/plugins/PluginProcessingCoordinator.cpp"
        "src/plugins/PluginProcessingCoordinator.h"
        "src/serato/SeratoPreviewLoader.cpp"
        "src/serato/SeratoPreviewLoader.h"
        "src/serato/SeratoWaveformColouring.cpp"
        "src/serato/SeratoWaveformColouring.h"
)

target_sources(AudioBatchCli PRIVATE ${AUDIOBATCH_CORE_SOURCES} ${AUDIOBATCH_CLI_SOURCES} "src/cli/CliMain.cpp")

# Project includes are written relative to src, for example "analysis/AudioAnalysis.h".
# The generated directory mirrors that layout for "utils/version.h".
target_include_directories(AudioBatch PRIVATE "${CMAKE_SOURCE_DIR}/src" "${AUDIOBATCH_GENERATED_DIR}")
target_include_directories(AudioBatchCli PRIVATE "${CMAKE_SOURCE_DIR}/src" "${AUDIOBATCH_GENERATED_DIR}")

target_compile_definitions(
    AudioBatch
    PRIVATE
        DONT_SET_USING_JUCE_NAMESPACE=1
        JUCE_ASIO=1
        JUCE_DIRECTSOUND=0
        # Debug builds would otherwise print the JUCE version on startup,
        # which pollutes the --version output.
        JUCE_DISABLE_JUCE_VERSION_PRINTING=1
        JUCE_USE_LAME_AUDIO_FORMAT=1
        JUCE_USE_MP3AUDIOFORMAT=1
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_AU=1
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JUCE_USE_CURL=0
        JUCE_USE_FLAC=1
        JUCE_USE_OGGVORBIS=1
        JUCE_USE_WINDOWS_MEDIA_FORMAT=0
        JUCE_WASAPI=0
        JUCE_WEB_BROWSER=0
        JUCE_WIN_PER_MONITOR_DPI_AWARE=1
)

# Compile definitions shared by the console executables, the CLI and the tests.
set(AUDIOBATCH_CONSOLE_DEFINITIONS
    DONT_SET_USING_JUCE_NAMESPACE=1
    JUCE_DISABLE_JUCE_VERSION_PRINTING=1
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_USE_CURL=0
    JUCE_USE_FLAC=1
    JUCE_USE_LAME_AUDIO_FORMAT=1
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_OGGVORBIS=1
    JUCE_USE_WINDOWS_MEDIA_FORMAT=0
    JUCE_WEB_BROWSER=0
)

target_compile_definitions(AudioBatchCli PRIVATE ${AUDIOBATCH_CONSOLE_DEFINITIONS})

set_target_properties(
    AudioBatch
    PROPERTIES
        COMPILE_WARNING_AS_ERROR YES
        CXX_STANDARD 23
        MACOSX_BUNDLE YES
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
        XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "YES"
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "7522H7K6WF"
        XCODE_GENERATE_SCHEME ON
)

set_target_properties(
    AudioBatchCli
    PROPERTIES
        COMPILE_WARNING_AS_ERROR YES
        CXX_STANDARD 23
        OUTPUT_NAME "audiobatch"
)

if(WIN32)
    # IFileOperation requires newer shell declarations than JUCE's default _WIN32_IE value.
    set_property(
        SOURCE src/utils/FileUtils.cpp
        APPEND
        PROPERTY COMPILE_DEFINITIONS _WIN32_IE=0x0700
    )
endif()

if(NOT MSVC)
    # Suppress -Wswitch-enum in vendored JUCE audio formats source (has a default case but not all enum values).
    set_source_files_properties(
        "${CMAKE_SOURCE_DIR}/JUCE/modules/juce_audio_formats/juce_audio_formats.mm"
        PROPERTIES COMPILE_OPTIONS "-Wno-switch-enum"
    )

    # Suppress -Wdeprecated-declarations in vendored JUCE audio processors source
    # (uses std::wstring_convert, deprecated in C++17 and flagged by macOS 26+ SDK).
    set_source_files_properties(
        "${CMAKE_SOURCE_DIR}/JUCE/modules/juce_audio_processors/juce_audio_processors.mm"
        "${CMAKE_SOURCE_DIR}/JUCE/modules/juce_audio_processors_headless/juce_audio_processors_headless.mm"
        PROPERTIES COMPILE_OPTIONS "-Wno-deprecated-declarations"
    )
endif()

# Applies the stricter warning set to the project's own sources only.
# JUCE module sources are compiled into the targets and are not written to these rules,
# so the options are attached per source file instead of per target.
# Every option is a front-end check, so the result does not depend on the optimization level.
function(audiobatch_set_source_warnings)
    set(project_sources "")
    foreach(target IN LISTS ARGN)
        get_target_property(target_sources ${target} SOURCES)
        foreach(source IN LISTS target_sources)
            if(source MATCHES "^(src|tests)/.*\\.cpp$")
                list(APPEND project_sources "${CMAKE_SOURCE_DIR}/${source}")
            endif()
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES project_sources)

    if(MSVC)
        set(options
            # Standards conformance mode.
            # https://learn.microsoft.com/en-us/cpp/build/reference/permissive-standards-conformance
            /permissive-
            # Source files and string literals are UTF-8, whatever code page the build machine uses.
            /utf-8
            # JUCE headers are written to JUCE's own rules, so they are treated as external headers
            # and their warnings are not reported, even when promoted to errors below.
            /external:I
            "${CMAKE_SOURCE_DIR}/JUCE/modules"
            /external:W0
            # Implicit conversions that lose data, change sign or sign-extend.
            /we4242
            /we4254
            /we4287
            /we4388
            /we4800
            /we4826
            # A virtual function that overrides nothing, and a polymorphic class without a virtual destructor.
            /we4263
            /we4265
            # Members initialized out of declaration order.
            /we5038
            # Unannotated switch fall-through.
            /we5262
            # Pointer truncation, and string literals cast to the wrong character type.
            /we4311
            /we4905
            /we4906
            # Expressions that are always false or have no effect, and misused comma operators.
            /we4296
            /we4545
            /we4546
            /we4547
            /we4549
            /we4555
            # A loop variable used outside its loop, and an illegal copy-initialization.
            /we4289
            /we4928
        )
    else()
        set(options
            # JUCE headers are written to JUCE's own rules, so they are treated as system headers.
            # Clang keeps the system classification when the same directory is also a normal include directory.
            -isystem
            "${CMAKE_SOURCE_DIR}/JUCE/modules"
            # The JUCE recommended warning flags include -Wall and -Wpedantic but not -Wextra.
            -Wextra
            # Implicit float to double widening, which is easy to introduce in single precision audio code.
            -Wdouble-promotion
            # Casts written the C way, and casts that drop qualifiers.
            -Wold-style-cast
            -Wcast-qual
            # Class hierarchies.
            -Wnon-virtual-dtor
            -Wsuggest-override
            # Unannotated switch fall-through.
            -Wimplicit-fallthrough
            -Wformat=2
            -Wnull-dereference
            # Dead code, stray semicolons and misused comma operators.
            -Wcomma
            -Wextra-semi-stmt
            -Wunreachable-code-aggressive
            -Wredundant-decls
            -Wmissing-declarations
            # A default label in a switch that already covers every enumerator hides future enumerators from -Wswitch.
            -Wcovered-switch-default
            # Namespace scope objects that run code during static initialization.
            -Wglobal-constructors
            # Unused code that is not caught by the default set.
            -Wunused-member-function
            -Wunused-template
            -Wunused-exception-parameter
        )
    endif()

    set_property(
        SOURCE ${project_sources}
        APPEND
        PROPERTY COMPILE_OPTIONS ${options}
    )
endfunction()

# Unit and integration tests for the shared core and the CLI front end, run through CTest.
if(BUILD_TESTING)
    include(CTest)
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

    # The VST3 test plugin shared with the Rust tests, which sets AUDIOBATCH_TEST_PLUGIN_BUNDLE to its bundle path.
    add_subdirectory(../test-plugin test-plugin)

    juce_add_console_app(
        AudioBatchTests
        PRODUCT_NAME "AudioBatchTests"
        VERSION ${APP_BUILD_VERSION}
    )
    juce_generate_juce_header(AudioBatchTests)

    target_sources(
        AudioBatchTests
        PRIVATE
            ${AUDIOBATCH_CORE_SOURCES}
            ${AUDIOBATCH_CLI_SOURCES}
            "src/gui/ArtworkLoader.cpp"
            "src/gui/ArtworkLoader.h"
            "src/gui/AudioPreviewPlayer.cpp"
            "src/gui/AudioPreviewPlayer.h"
            "src/gui/AudioBatchComponentState.h"
            "src/gui/BackgroundWorkController.cpp"
            "src/gui/BackgroundWorkController.h"
            "src/gui/components/AudioInfoPanel.cpp"
            "src/gui/components/AudioInfoPanel.h"
            "src/gui/components/AudioSettingsComponent.cpp"
            "src/gui/components/AudioSettingsComponent.h"
            "src/gui/components/PluginChainEditor.cpp"
            "src/gui/components/PluginChainEditor.h"
            "src/gui/components/CustomLookAndFeel.cpp"
            "src/gui/components/CustomLookAndFeel.h"
            "src/gui/components/StatusBar.cpp"
            "src/gui/components/StatusBar.h"
            "src/gui/components/ThumbnailComponent.cpp"
            "src/gui/components/ThumbnailComponent.h"
            "src/gui/FileActivityTracker.cpp"
            "src/gui/FileActivityTracker.h"
            "src/gui/LiveChain.cpp"
            "src/gui/LiveChain.h"
            "src/gui/LiveChainPreview.cpp"
            "src/gui/LiveChainPresets.cpp"
            "src/gui/LiveChainWindows.cpp"
            "src/gui/PreviewController.cpp"
            "src/gui/PreviewController.h"
            "src/gui/ResultsModel.cpp"
            "src/gui/ResultsModel.h"
            "src/gui/ResultsSorting.cpp"
            "src/gui/ResultsSorting.h"
            "src/plugins/PluginChainPreparation.cpp"
            "src/plugins/PluginLibrary.cpp"
            "src/plugins/PluginLibrary.h"
            "src/plugins/PluginPreviewSource.cpp"
            "src/plugins/PluginPreviewSource.h"
            "src/plugins/PluginChainPreparation.h"
            "src/plugins/PluginChainPresets.cpp"
            "src/plugins/PluginChainPresets.h"
            "src/plugins/PluginProcessing.cpp"
            "src/plugins/PluginProcessing.h"
            "src/plugins/PluginProcessingCoordinator.cpp"
            "src/plugins/PluginProcessingCoordinator.h"
            "src/serato/SeratoPreviewLoader.cpp"
            "src/serato/SeratoPreviewLoader.h"
            "src/serato/SeratoWaveformColouring.cpp"
            "src/serato/SeratoWaveformColouring.h"
            "tests/AnalysisCoordinatorTests.cpp"
            "tests/AnalysisTests.cpp"
            "tests/ArtworkTests.cpp"
            "tests/BackgroundWorkTests.cpp"
            "tests/CacheTests.cpp"
            "tests/CliTests.cpp"
            "tests/FileActivityTests.cpp"
            "tests/FormatTests.cpp"
            "tests/GuiFontTests.cpp"
            "tests/MetadataTests.cpp"
            "tests/NormalizationTests.cpp"
            "tests/NormalizeCoordinatorTests.cpp"
            "tests/OverloadHistoryTests.cpp"
            "tests/PluginCoordinatorTests.cpp"
            "tests/PluginPresetTests.cpp"
            "tests/PluginScanTests.cpp"
            "tests/ProcessingTestPlugins.cpp"
            "tests/ProcessingTestPlugins.h"
            "tests/ProcessingTests.cpp"
            "tests/PreviewControllerTests.cpp"
            "tests/PreviewPlaybackTests.cpp"
            "tests/PreviewPlayerTests.cpp"
            "tests/ResultsModelTests.cpp"
            "tests/SeratoOverviewTests.cpp"
            "tests/SeratoPreviewLoaderTests.cpp"
            "tests/SeratoPreviewTests.cpp"
            "tests/SeratoTagReaderTests.cpp"
            "tests/SeratoTagsTests.cpp"
            "tests/SortingTests.cpp"
            "tests/TestHelpers.cpp"
            "tests/TestHelpers.h"
            "tests/TestMain.cpp"
            "tests/TestSuites.h"
            "tests/UtilsTests.cpp"
    )

    target_include_directories(AudioBatchTests PRIVATE "${CMAKE_SOURCE_DIR}/src" "${AUDIOBATCH_GENERATED_DIR}")
    target_link_libraries(AudioBatchTests PRIVATE Assets)
    target_compile_definitions(
        AudioBatchTests
        PRIVATE ${AUDIOBATCH_CONSOLE_DEFINITIONS} AUDIOBATCH_TEST_FIXTURES_DIR="${AUDIOBATCH_FIXTURES_DIR}"
                AUDIOBATCH_TEST_VST3_PATH="${AUDIOBATCH_TEST_PLUGIN_BUNDLE}"
    )
    add_dependencies(AudioBatchTests AudioBatchTestPlugin_VST3)
    set_target_properties(AudioBatchTests PROPERTIES COMPILE_WARNING_AS_ERROR YES CXX_STANDARD 23)

    if(APPLE)
        target_link_libraries(
            AudioBatchTests PRIVATE "$<LINK_LIBRARY:FRAMEWORK,AudioUnit>" "$<LINK_LIBRARY:FRAMEWORK,CoreAudioKit>"
        )
    endif()

    # cmake-format: off
    target_link_libraries(
        AudioBatchTests
        PRIVATE
            GTest::gtest
            ${AUDIOBATCH_SQLITE_TARGET}
            ebur128
            fmt::fmt
            tag
            juce::juce_audio_basics
            juce::juce_audio_formats
            juce::juce_audio_processors
            juce::juce_audio_utils
            juce::juce_core
            juce::juce_data_structures
            juce::juce_events
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags
    )
    # cmake-format: on

    # Added before the discovery step below, so the runtime is in place when discovery runs the executable.
    if(AUDIOBATCH_ASAN_RUNTIME)
        add_custom_command(
            TARGET AudioBatchTests
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${AUDIOBATCH_ASAN_RUNTIME} $<TARGET_FILE_DIR:AudioBatchTests>
            VERBATIM
        )
    endif()

    include(GoogleTest)
    # Discovery runs the freshly linked executable as a post-build step.
    # The first launch of a new binary can take longer than the default five seconds,
    # for example while macOS scans it or a sanitizer runtime starts up.
    # Each suite takes a few seconds in a debug build, so the timeout of each test only ends a suite that hangs,
    # with room for the slower sanitizer builds.
    gtest_discover_tests(
        AudioBatchTests DISCOVERY_TIMEOUT 60
        PROPERTIES RUN_SERIAL TRUE
        TIMEOUT 300
    )
    audiobatch_set_source_warnings(AudioBatchTests)
endif()

# libFuzzer harnesses for the parsers that read untrusted input, built by the macos-fuzz preset.
# They need a Clang that ships libFuzzer, such as Homebrew LLVM, because Apple Clang does not.
# CTest runs each harness once over its committed corpus, and fuzzing is started by running a harness directly.
option(AUDIOBATCH_FUZZ "Build the libFuzzer harnesses" OFF)

if(AUDIOBATCH_FUZZ)
    enable_testing()
    set(AUDIOBATCH_FUZZERS SeratoTagFuzzer CliArgumentsFuzzer)

    foreach(fuzzer IN LISTS AUDIOBATCH_FUZZERS)
        juce_add_console_app(${fuzzer} PRODUCT_NAME ${fuzzer})
        juce_generate_juce_header(${fuzzer})
        target_sources(
            ${fuzzer} PRIVATE ${AUDIOBATCH_CORE_SOURCES} ${AUDIOBATCH_CLI_SOURCES} "tests/fuzz/${fuzzer}.cpp"
        )
        target_include_directories(${fuzzer} PRIVATE "${CMAKE_SOURCE_DIR}/src" "${AUDIOBATCH_GENERATED_DIR}")
        target_compile_definitions(${fuzzer} PRIVATE ${AUDIOBATCH_CONSOLE_DEFINITIONS})
        target_compile_options(${fuzzer} PRIVATE -fsanitize=fuzzer)
        target_link_options(${fuzzer} PRIVATE -fsanitize=fuzzer)
        set_target_properties(${fuzzer} PROPERTIES COMPILE_WARNING_AS_ERROR YES CXX_STANDARD 23)

        if(APPLE)
            target_link_libraries(
                ${fuzzer} PRIVATE "$<LINK_LIBRARY:FRAMEWORK,AudioUnit>" "$<LINK_LIBRARY:FRAMEWORK,CoreAudioKit>"
            )
        endif()

        # cmake-format: off
        target_link_libraries(
            ${fuzzer}
            PRIVATE
                ${AUDIOBATCH_SQLITE_TARGET}
                ebur128
                fmt::fmt
                tag
                juce::juce_audio_basics
                juce::juce_audio_formats
                juce::juce_audio_processors
                juce::juce_core
                juce::juce_data_structures
                juce::juce_events
            PUBLIC
                juce::juce_recommended_config_flags
                juce::juce_recommended_warning_flags
        )
        # cmake-format: on

        add_test(NAME ${fuzzer}Corpus COMMAND ${fuzzer} -runs=0 "${CMAKE_SOURCE_DIR}/tests/fuzz/corpus/${fuzzer}")
    endforeach()

    audiobatch_set_source_warnings(${AUDIOBATCH_FUZZERS})
endif()

audiobatch_set_source_warnings(AudioBatch AudioBatchCli)

# Mirror the src subdirectories as folders in the Visual Studio and Xcode projects.
foreach(target IN ITEMS AudioBatch AudioBatchCli AudioBatchTests)
    if(TARGET ${target})
        get_target_property(target_sources ${target} SOURCES)
        list(FILTER target_sources INCLUDE REGEX "^src/")
        source_group(TREE "${CMAKE_SOURCE_DIR}" FILES ${target_sources})
    endif()
endforeach()

juce_add_binary_data(
    Assets
    NAMESPACE BinaryData
    SOURCES "${AUDIOBATCH_ASSETS_DIR}/RobotoMono-Regular.ttf" "${AUDIOBATCH_ASSETS_DIR}/Roboto-Regular.ttf"
)

# The Audio Unit hosting code needs these frameworks for the plugin listing.
# The GUI gets CoreAudioKit through juce_audio_utils instead.
if(APPLE)
    target_link_libraries(
        AudioBatchCli PRIVATE "$<LINK_LIBRARY:FRAMEWORK,AudioUnit>" "$<LINK_LIBRARY:FRAMEWORK,CoreAudioKit>"
    )
endif()

# Keep one library per line, which cmake-format would otherwise join.
# cmake-format: off
target_link_libraries(
    AudioBatch
    PRIVATE
        Assets
        ${AUDIOBATCH_SQLITE_TARGET}
        ebur128
        fmt::fmt
        tag
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
# cmake-format: on

# cmake-format: off
target_link_libraries(
    AudioBatchCli
    PRIVATE
        ${AUDIOBATCH_SQLITE_TARGET}
        ebur128
        fmt::fmt
        tag
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_core
        juce::juce_data_structures
        juce::juce_events
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
# cmake-format: on

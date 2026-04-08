# AudioBatch
# Akseli Lukkarila
# 2023-2025

cmake_minimum_required(VERSION 4.2)

project("AudioBatch"
    VERSION 0.4.0
    DESCRIPTION "App for batch processing audio files"
    HOMEPAGE_URL "https://github.com/Esgrove/AudioBatch"
    LANGUAGES CXX C
)

include(FetchContent)
include(GNUInstallDirs)

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
    COMMAND bash -c "git rev-parse --short HEAD"
    OUTPUT_VARIABLE GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
# Git branch
execute_process(
    COMMAND bash -c "git branch --show-current"
    OUTPUT_VARIABLE GIT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
# Git tag
execute_process(
    COMMAND bash -c "git describe --abbrev=0"
    OUTPUT_VARIABLE GIT_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
# Build number
execute_process(
    COMMAND bash -c "git rev-list ${GIT_TAG}..HEAD --count"
    OUTPUT_VARIABLE BUILD_NUMBER
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Date and time
string(TIMESTAMP DATE "%Y-%m-%d" UTC)
string(TIMESTAMP TIMESTAMP "%Y-%m-%d_%H%M" UTC)

set(APP_BUILD_VERSION "${CMAKE_PROJECT_VERSION}.${BUILD_NUMBER}")
set(APP_BUILD_NAME "${CMAKE_PROJECT_NAME}_${APP_BUILD_VERSION}_${GIT_HASH}_${TIMESTAMP}")

message(STATUS "Build name: ${APP_BUILD_NAME}")

if(WIN32)
    set(ICON_FILE "${CMAKE_SOURCE_DIR}/Assets/icon_win.png")
else()
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum macOS version")
    #set(CMAKE_OSX_ARCHITECTURES arm64 x86_64)
    # Xcode: Disable automatic build scheme generation.
    # Instead, explicitly specify which targets require schemes.
    set(CMAKE_XCODE_GENERATE_SCHEME OFF)
    set(CMAKE_XCODE_ATTRIBUTE_DEAD_CODE_STRIPPING YES)
    set(ICON_FILE "${CMAKE_SOURCE_DIR}/Assets/icon_mac.png")
endif()

# Adds all the module sources so they appear correctly in the IDE
# Must be set before JUCE is added as a sub-dir (or any targets are made)
# https://github.com/juce-framework/JUCE/commit/6b1b4cf7f6b1008db44411f2c8887d71a3348889
set_property(GLOBAL PROPERTY USE_FOLDERS YES)

# Create a "/Modules" directory in the IDE with the JUCE Module code
option(JUCE_ENABLE_MODULE_SOURCE_GROUPS "Show all module sources in IDE projects" ON)

add_subdirectory(JUCE)

find_package(SQLite3 QUIET)

if (SQLite3_FOUND)
    set(AUDIOBATCH_SQLITE_TARGET SQLite::SQLite3)
else()
    FetchContent_Declare(sqlite3
        URL https://www.sqlite.org/2026/sqlite-amalgamation-3510300.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_GetProperties(sqlite3)

    if (NOT sqlite3_POPULATED)
        cmake_policy(PUSH)
        cmake_policy(SET CMP0169 OLD)
        FetchContent_Populate(sqlite3)
        cmake_policy(POP)
    endif()

    add_library(AudioBatchSQLite STATIC
        ${sqlite3_SOURCE_DIR}/sqlite3.c
    )

    target_compile_definitions(AudioBatchSQLite
        PRIVATE
            SQLITE_DQS=0
            SQLITE_OMIT_LOAD_EXTENSION=1
            SQLITE_THREADSAFE=1
    )

    target_include_directories(AudioBatchSQLite
        PUBLIC
            ${sqlite3_SOURCE_DIR}
    )

    set(AUDIOBATCH_SQLITE_TARGET AudioBatchSQLite)
endif()

FetchContent_Declare(libebur128
    GIT_REPOSITORY https://github.com/jiixyj/libebur128.git
    GIT_TAG v1.2.6
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(libebur128)

if (NOT libebur128_POPULATED)
    cmake_policy(PUSH)
    cmake_policy(SET CMP0169 OLD)
    FetchContent_Populate(libebur128)
    cmake_policy(POP)

    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
    add_subdirectory(${libebur128_SOURCE_DIR}/ebur128 ${libebur128_BINARY_DIR}/ebur128 EXCLUDE_FROM_ALL)
endif()

target_include_directories(ebur128
    PUBLIC
        ${libebur128_SOURCE_DIR}/ebur128
)

juce_add_gui_app(AudioBatch
    BUNDLE_ID com.esgrove.audiobatch
    COMPANY_NAME "Esgrove"
    COMPANY_WEBSITE "https://github.com/esgrove"
    ICON_BIG ${ICON_FILE}
    PRODUCT_NAME "AudioBatch"
    VERSION ${APP_BUILD_VERSION}
    PLIST_TO_MERGE "<plist version=\"1.0\">
        <dict>
            <key>CFBundleIconFile</key>
            <string>Icon.icns</string>
            <key>LSApplicationCategoryType</key>
            <string>public.app-category.developer-tools</string>
        </dict>
        </plist>"
)

juce_add_console_app(AudioBatchCli
    COMPANY_NAME "Esgrove"
    COMPANY_WEBSITE "https://github.com/esgrove"
    PRODUCT_NAME "audiobatch"
    VERSION ${APP_BUILD_VERSION}
)

juce_generate_juce_header(AudioBatch)
juce_generate_juce_header(AudioBatchCli)

target_sources(AudioBatch
    PRIVATE
        "src/AnalysisCache.cpp"
        "src/AnalysisCache.h"
        "src/AnalysisCoordinator.cpp"
        "src/AnalysisCoordinator.h"
        "src/AudioAnalysisCli.cpp"
        "src/AudioAnalysisCli.h"
        "src/AudioAnalysisService.cpp"
        "src/AudioAnalysisService.h"
        "src/AudioAnalysisTypes.h"
        "src/AudioBatchComponent.cpp"
        "src/AudioBatchComponent.h"
        "src/AudioFileTableModel.cpp"
        "src/AudioFileTableModel.h"
        "src/AudioNormalizationService.cpp"
        "src/AudioNormalizationService.h"
        "src/CustomLookAndFeel.cpp"
        "src/CustomLookAndFeel.h"
        "src/Main.cpp"
        "src/NormalizeCoordinator.cpp"
        "src/NormalizeCoordinator.h"
        "src/ThumbnailComponent.cpp"
        "src/ThumbnailComponent.h"
        "src/utils.cpp"
        "src/utils.h"
)

target_include_directories(AudioBatch
    PRIVATE
        "include/ASIO_SDK"
)

target_sources(AudioBatchCli
    PRIVATE
        "src/AnalysisCache.cpp"
        "src/AnalysisCache.h"
        "src/AnalysisCoordinator.cpp"
        "src/AnalysisCoordinator.h"
        "src/AudioAnalysisCli.cpp"
        "src/AudioAnalysisCli.h"
        "src/AudioAnalysisService.cpp"
        "src/AudioAnalysisService.h"
        "src/AudioAnalysisTypes.h"
        "src/AudioNormalizationService.cpp"
        "src/AudioNormalizationService.h"
        "src/CliMain.cpp"
        "src/NormalizeCoordinator.cpp"
        "src/NormalizeCoordinator.h"
        "src/utils.cpp"
        "src/utils.h"
        "src/version.h"
)

target_compile_definitions(AudioBatch
    PRIVATE
        DONT_SET_USING_JUCE_NAMESPACE=1
        JUCE_ASIO=1
        JUCE_DIRECTSOUND=0
        JUCE_USE_LAME_AUDIO_FORMAT=1
        JUCE_USE_MP3AUDIOFORMAT=1
        JUCE_PLUGINHOST_VST3=1
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JUCE_USE_CURL=0
        JUCE_USE_FLAC=1
        JUCE_USE_OGGVORBIS=1
        JUCE_USE_WINDOWS_MEDIA_FORMAT=0
        JUCE_WASAPI=0
        JUCE_WEB_BROWSER=0
        JUCE_WIN_PER_MONITOR_DPI_AWARE=0
        # Version info
        BUILDTIME_APP_NAME="${CMAKE_PROJECT_NAME}"
        BUILDTIME_BRANCH="${GIT_BRANCH}"
        BUILDTIME_BUILD_NAME="${APP_BUILD_NAME}"
        BUILDTIME_COMMIT="${GIT_HASH}"
        BUILDTIME_DATE="${DATE}"
        BUILDTIME_VERSION_NUMBER="${APP_BUILD_VERSION}"
        BUILDTIME_VERSION_INFO="${APP_BUILD_VERSION} ${DATE} ${GIT_HASH}"
)

target_compile_definitions(AudioBatchCli
    PRIVATE
        DONT_SET_USING_JUCE_NAMESPACE=1
        JUCE_USE_LAME_AUDIO_FORMAT=1
        JUCE_USE_MP3AUDIOFORMAT=1
        JUCE_USE_CURL=0
        JUCE_USE_FLAC=1
        JUCE_USE_OGGVORBIS=1
        JUCE_USE_WINDOWS_MEDIA_FORMAT=0
        # Version info
        BUILDTIME_APP_NAME="${CMAKE_PROJECT_NAME}"
        BUILDTIME_BRANCH="${GIT_BRANCH}"
        BUILDTIME_BUILD_NAME="${APP_BUILD_NAME}"
        BUILDTIME_COMMIT="${GIT_HASH}"
        BUILDTIME_DATE="${DATE}"
        BUILDTIME_VERSION_NUMBER="${APP_BUILD_VERSION}"
        BUILDTIME_VERSION_INFO="${APP_BUILD_VERSION} ${DATE} ${GIT_HASH}"
)

set_target_properties(AudioBatch
    PROPERTIES
        COMPILE_WARNING_AS_ERROR YES
        CXX_STANDARD 23
        MACOSX_BUNDLE YES
        OUTPUT_NAME "AudioBatchApp"
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
        XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "YES"
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "7522H7K6WF"
        XCODE_GENERATE_SCHEME ON
)

set_target_properties(AudioBatchCli
    PROPERTIES
        COMPILE_WARNING_AS_ERROR YES
        CXX_STANDARD 23
        OUTPUT_NAME "audiobatch"
)

if (MSVC)
    # /wd4100 disables MSVC warning C4100 (unreferenced formal parameter) for this JUCE source file only.
    set_source_files_properties(
        "${CMAKE_SOURCE_DIR}/JUCE/modules/juce_gui_basics/juce_gui_basics.cpp"
        PROPERTIES
            COMPILE_OPTIONS "/wd4100"
    )

    # https://docs.microsoft.com/en-us/cpp/build/reference/permissive-standards-conformance
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -pedantic)
    add_link_options(-Wl,-weak_reference_mismatches,weak)
endif()

juce_add_binary_data(Assets NAMESPACE BinaryData
    SOURCES
        "assets/RobotoMono-Regular.ttf"
        "assets/Roboto-Regular.ttf"
)

target_link_libraries(AudioBatch
    PRIVATE
        Assets
        ${AUDIOBATCH_SQLITE_TARGET}
        ebur128
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

target_link_libraries(AudioBatchCli
    PRIVATE
        ${AUDIOBATCH_SQLITE_TARGET}
        ebur128
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_core
        juce::juce_data_structures
        juce::juce_events
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

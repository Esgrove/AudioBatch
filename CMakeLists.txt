# AudioBatch
# Akseli Lukkarila
# 2023

cmake_minimum_required(VERSION 3.18)

project("AudioBatch"
    VERSION 0.1
    DESCRIPTION "App for batch processing audio files"
    HOMEPAGE_URL "https://github.com/Esgrove/AudioBatch"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
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
    # Note: WIN32 is true on Windows for x64 as well
    set(CMAKE_GENERATOR_PLATFORM "x64")
    set(ICON_FILE "${CMAKE_SOURCE_DIR}/Assets/icon_win.png")
else()
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.14")
    set(CMAKE_OSX_ARCHITECTURES arm64 x86_64)
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

juce_generate_juce_header(AudioBatch)

target_sources(AudioBatch
    PRIVATE
        "src/AudioBatchComponent.cpp"
        "src/AudioBatchComponent.h"
        "src/CustomLookAndFeel.cpp"
        "src/CustomLookAndFeel.h"
        "src/Main.cpp"
        "src/ThumbnailComponent.cpp"
        "src/ThumbnailComponent.h"
        "src/utils.cpp"
        "src/utils.h"
)

target_include_directories(AudioBatch
    PRIVATE
        "include/ASIO_SDK"
)

target_compile_definitions(AudioBatch
    PRIVATE
        DONT_SET_USING_JUCE_NAMESPACE=1
        JUCE_ASIO=1
        JUCE_DIRECTSOUND=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_PLUGINHOST_VST3=1
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JUCE_USE_CURL=0
        JUCE_USE_FLAC=1
        JUCE_USE_OGGVORBIS=1
        JUCE_USE_WINDOWS_MEDIA_FORMAT=1
        JUCE_WASAPI=1
        JUCE_WEB_BROWSER=0
        JUCE_WIN_PER_MONITOR_DPI_AWARE=0
        # Version info
        BUILDTIME_APP_NAME="${CMAKE_PROJECT_NAME}"
        BUILDTIME_BRANCH="${GIT_BRANCH}"
        BUILDTIME_BUILD_NAME="${APP_BUILD_NAME}"
        BUILDTIME_COMMIT="${GIT_HASH}"
        BUILDTIME_DATE="${DATE}"
        BUILDTIME_VERSION_NUMBER="${APP_BUILD_VERSION}"
        BUILDTIME_VERSION_INFO="${APP_BUILD_VERSION} ${GIT_HASH} ${DATE}"
)

set_target_properties(AudioBatch
    PROPERTIES
        CXX_STANDARD 20
        MACOSX_BUNDLE YES
        OUTPUT_NAME "${CMAKE_PROJECT_NAME}"
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
        XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "YES"
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "7522H7K6WF"
        XCODE_GENERATE_SCHEME ON
)

if (MSVC)
    # https://docs.microsoft.com/en-us/cpp/build/reference/permissive-standards-conformance
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -pedantic)
endif()

juce_add_binary_data(Assets NAMESPACE BinaryData
    SOURCES
        "assets/RobotoMono-Regular.ttf"
        "assets/Roboto-Regular.ttf"
)

target_link_libraries(AudioBatch
    PRIVATE
        Assets
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

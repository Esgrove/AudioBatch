#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS]

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -n | --ninja
        Use Ninja as the build system.

    -b | --build-type <type>
        Specify build type for CMake. Default is 'Release'.

    -v | --verbose
        Display commands being executed.
"

# Import common functions
DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

init_options() {
    BUILD_TYPE="Release"
    USE_NINJA=false

    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help)
                echo "$USAGE"
                exit 1
                ;;
            -n | --ninja)
                USE_NINJA="true"
                ;;
            -b | --build-type)
                BUILD_TYPE="$2"
                shift
                ;;
            -v | --verbose)
                set -x
                ;;
        esac
        shift
    done

    JUCE_PATH="$REPO/JUCE"
    APP_NAME="AudioBatch"
    GUI_TARGET_NAME="AudioBatch"
    GUI_BINARY_NAME="AudioBatchApp"
    CLI_TARGET_NAME="AudioBatchCli"
    CLI_BINARY_NAME="audiobatch"
    CMAKE_BUILD_DIR="$REPO/cmake-build-${BASH_PLATFORM}-$(echo "$BUILD_TYPE" | tr '[:upper:]' '[:lower:]')"
}

require_path() {
    if [ ! -e "$1" ]; then
        print_error_and_exit "Expected build artifact not found: $1"
    fi
}

find_first_path() {
    local search_root="$1"
    shift
    find "$search_root" "$@" -print -quit
}

build_app() {
    print_magenta "Building $APP_NAME..."
    if [ "$BASH_PLATFORM" = "mac" ]; then
        build_mac_app
    elif [ "$BASH_PLATFORM" = "windows" ]; then
        build_windows_app
    else
        print_error_and_exit "Platform is not supported yet"
    fi
}

build_mac_app() {
    GUI_APP_BUNDLE="$GUI_BINARY_NAME.app"
    GUI_APP_DESTINATION="$REPO/$GUI_APP_BUNDLE"
    CLI_APP_DESTINATION="$REPO/$CLI_BINARY_NAME"

    rm -rf "${GUI_APP_DESTINATION:?}"
    rm -f "$CLI_APP_DESTINATION"

    if [ "$USE_NINJA" = true ]; then
        cmake --build "$CMAKE_BUILD_DIR" --target "$GUI_TARGET_NAME" "$CLI_TARGET_NAME" --config "$BUILD_TYPE"
    else
        if [ -n "$(command -v xcbeautify)" ]; then
            # nicer xcodebuild output: https://github.com/tuist/xcbeautify
            time cmake --build "$CMAKE_BUILD_DIR" --target "$GUI_TARGET_NAME" "$CLI_TARGET_NAME" --config "$BUILD_TYPE" | xcbeautify
        else
            print_yellow "xcbeautify missing, install it with brew"
            time cmake --build "$CMAKE_BUILD_DIR" --target "$GUI_TARGET_NAME" "$CLI_TARGET_NAME" --config "$BUILD_TYPE"
        fi
    fi

    GUI_APP_SOURCE=$(find_first_path "$CMAKE_BUILD_DIR" -type d -name "$GUI_APP_BUNDLE")
    CLI_APP_SOURCE=$(find_first_path "$CMAKE_BUILD_DIR" -type f -path "*/$BUILD_TYPE/$CLI_BINARY_NAME")
    require_path "$GUI_APP_SOURCE"
    require_path "$CLI_APP_SOURCE"

    GUI_EXECUTABLE="$GUI_APP_DESTINATION/Contents/MacOS/$GUI_BINARY_NAME"
    mv -f "$GUI_APP_SOURCE" "$GUI_APP_DESTINATION"
    cp -f "$CLI_APP_SOURCE" "$CLI_APP_DESTINATION"

    file "$GUI_EXECUTABLE"
    file "$CLI_APP_DESTINATION"
    print_green "GUI build successful: $GUI_APP_BUNDLE"
    print_green "CLI build successful: $CLI_BINARY_NAME"
    "$CLI_APP_DESTINATION" --version
}

build_windows_app() {
    GUI_EXE="$GUI_BINARY_NAME.exe"
    CLI_EXE="$CLI_BINARY_NAME.exe"
    GUI_EXECUTABLE_SOURCE="$CMAKE_BUILD_DIR/AudioBatch_artefacts/$BUILD_TYPE/$GUI_EXE"
    CLI_EXECUTABLE_SOURCE="$CMAKE_BUILD_DIR/AudioBatchCli_artefacts/$BUILD_TYPE/$CLI_EXE"
    GUI_EXECUTABLE="$REPO/$GUI_EXE"
    CLI_EXECUTABLE="$REPO/$CLI_EXE"

    rm -f "$GUI_EXECUTABLE" "$CLI_EXECUTABLE"

    cmake --build "$CMAKE_BUILD_DIR" --target "$GUI_TARGET_NAME" "$CLI_TARGET_NAME" --config "$BUILD_TYPE"

    require_path "$GUI_EXECUTABLE_SOURCE"
    require_path "$CLI_EXECUTABLE_SOURCE"

    cp -f "$GUI_EXECUTABLE_SOURCE" "$GUI_EXECUTABLE"
    cp -f "$CLI_EXECUTABLE_SOURCE" "$CLI_EXECUTABLE"

    file "$GUI_EXECUTABLE"
    file "$CLI_EXECUTABLE"
    print_green "GUI build successful: $GUI_EXE"
    print_green "CLI build successful: $CLI_EXE"
    "$CLI_EXECUTABLE" --version
}

check_juce_submodule() {
    # Update JUCE submodule if needed
    print_magenta "Checking JUCE submodule..."
    if [ ! -d "$JUCE_PATH/modules" ]; then
        print_yellow "JUCE directory not found, cloning submodule..."
        git submodule update --init --recursive
    fi
    JUCE_VERSION="$(git submodule status | grep JUCE)"
    CACHED_JUCE_VERSION="$(git submodule status --cached | grep JUCE)"
    if [ "$JUCE_VERSION" != "$CACHED_JUCE_VERSION" ]; then
        print_yellow "JUCE submodule out of date, updating..."
        git submodule update --init --recursive
    else
        echo "up to date"
    fi
}

export_cmake_project() {
    # Export Xcode / VS project from CMake
    cd "$REPO" || print_error_and_exit "Failed to cd to repo root"

    if [ "$USE_NINJA" = true ]; then
        print_magenta "Generating Ninja project..."
        if ! cmake -B "$CMAKE_BUILD_DIR" -G Ninja; then
            rm -rf "$CMAKE_BUILD_DIR"
            cmake -B "$CMAKE_BUILD_DIR" -G Ninja
        fi
    else
        print_magenta "Generating IDE project..."
        if [ "$BASH_PLATFORM" = "windows" ]; then
            if ! cmake -B "$CMAKE_BUILD_DIR" -G "Visual Studio 18 2026" -A x64; then
                rm -rf "$CMAKE_BUILD_DIR"
                cmake -B "$CMAKE_BUILD_DIR" -G "Visual Studio 18 2026" -A x64
            fi
        elif [ "$BASH_PLATFORM" = "mac" ]; then
            if ! cmake -B "$CMAKE_BUILD_DIR" -G "Xcode"; then
                rm -rf "$CMAKE_BUILD_DIR"
                cmake -B "$CMAKE_BUILD_DIR" -G "Xcode"
            fi
        else
            print_error_and_exit "Platform is not supported yet"
        fi
    fi
}

init_options "$@"
check_juce_submodule
export_cmake_project
build_app
